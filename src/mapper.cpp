#include <unordered_set>
#include "mapper.hpp"

//#define debug_mapper

namespace vg {

Mapper::Mapper(Index* idex,
               xg::XG* xidex,
               gcsa::GCSA* g,
               gcsa::LCPArray* a)
    : index(idex)
    , xindex(xidex)
    , gcsa(g)
    , lcp(a)
    , best_clusters(0)
    , cluster_min(1)
    , hit_max(0)
    , hit_size_threshold(512)
    , kmer_min(0)
    , kmer_sensitivity_step(5)
    , thread_extension(10)
    , max_thread_gap(30)
    , context_depth(1)
    , max_multimaps(1)
    , min_multimaps(1)
    , max_attempts(0)
    , softclip_threshold(0)
    , max_softclip_iterations(10)
    , prefer_forward(false)
    , greedy_accept(false)
    , accept_identity(0.75)
    , min_identity(0)
    , min_kmer_entropy(0)
    , debug(false)
    , alignment_threads(1)
    , min_mem_length(0)
    , mem_chaining(false)
    , fast_reseed(true)
    , max_target_factor(128)
    , max_query_graph_ratio(128)
    , extra_multimaps(512)
    , always_rescue(false)
    , fragment_size(0)
    , fragment_max(1e4)
    , fragment_sigma(10)
    , fragment_length_cache_size(1000)
    , cached_fragment_length_mean(0)
    , cached_fragment_length_stdev(0)
    , cached_fragment_orientation(0)
    , cached_fragment_direction(1)
    , since_last_fragment_length_estimate(0)
    , fragment_length_estimate_interval(10)
    , perfect_pair_identity_threshold(0.95)
    , mapping_quality_method(Approx)
    , adjust_alignments_for_base_quality(false)
    , max_mapping_quality(60)
    , max_cluster_mapping_quality(1024)
    , mem_reseed_length(0)
    , use_cluster_mq(false)
    , simultaneous_pair_alignment(true)
    , drop_chain(0.2)
    , mq_overlap(0.2)
    , cache_size(128)
    , mate_rescues(32)
    , alignment_match(1)
    , alignment_mismatch(4)
    , alignment_gap_open(6)
    , alignment_gap_extension(1)
    , full_length_alignment_bonus(5)
    , maybe_mq_threshold(10)
{
    init_aligner(alignment_match, alignment_mismatch, alignment_gap_open, alignment_gap_extension);
    init_node_cache();
    init_node_start_cache();
    init_node_pos_cache();
    init_edge_cache();
}

Mapper::Mapper(Index* idex, gcsa::GCSA* g, gcsa::LCPArray* a) : Mapper(idex, nullptr, g, a)
{
    if(idex == nullptr) {
        // With this constructor we need an index.
        cerr << "error:[vg::Mapper] cannot create a RocksDB-based Mapper with null index" << endl;
        exit(1);
    }

    kmer_sizes = index->stored_kmer_sizes();
    if (kmer_sizes.empty() && gcsa == NULL) {
        cerr << "error:[vg::Mapper] the index ("
             << index->name << ") does not include kmers"
             << " and no GCSA index has been provided" << endl;
        exit(1);
    }
}

Mapper::Mapper(xg::XG* xidex, gcsa::GCSA* g, gcsa::LCPArray* a) : Mapper(nullptr, xidex, g, a) {
    if(xidex == nullptr) {
        // With this constructor we need an XG graph.
        cerr << "error:[vg::Mapper] cannot create an xg-based Mapper with null xg index" << endl;
        exit(1);
    }

    if(g == nullptr || a == nullptr) {
        // With this constructor we need a GCSA2 index too.
        cerr << "error:[vg::Mapper] cannot create an xg-based Mapper with null GCSA2 index" << endl;
        exit(1);
    }
}

Mapper::Mapper(void) : Mapper(nullptr, nullptr, nullptr, nullptr) {
    // Nothing to do. Default constructed and can't really do anything.
}

Mapper::~Mapper(void) {
    for (auto& aligner : qual_adj_aligners) {
        delete aligner;
    }
    for (auto& aligner : regular_aligners) {
        delete aligner;
    }
    for (auto& nc : node_cache) {
        delete nc;
    }
    for (auto& np : node_pos_cache) {
        delete np;
    }
}
    
double Mapper::estimate_gc_content(void) {
    
    uint64_t at = 0, gc = 0;
    
    if (gcsa) {
        at = gcsa::Range::length(gcsa->find(string("A"))) + gcsa::Range::length(gcsa->find(string("T")));
        gc = gcsa::Range::length(gcsa->find(string("G"))) + gcsa::Range::length(gcsa->find(string("C")));
    }
    else if (index) {
        at = index->approx_size_of_kmer_matches("A") + index->approx_size_of_kmer_matches("T");
        gc = index->approx_size_of_kmer_matches("G") + index->approx_size_of_kmer_matches("C");
    }

    if (at == 0 || gc == 0) {
        return default_gc_content;
    }
    
    return ((double) gc) / (at + gc);
}

int Mapper::random_match_length(double chance_random) {
    size_t length = 0;
    if (xindex) {
        length = xindex->seq_length;
    } else if (index) {
        length = index->approx_size_of_kmer_matches("");
    } else {
        return 0;
    }
    return ceil(- (log(1.0 - pow(pow(1.0-chance_random, -1), (-1.0/length))) / log(4.0)));
}

double Mapper::graph_entropy(void) {
    const size_t seq_bytes = xindex->sequence_bit_size() / 8;
    char* seq = (char*) xindex->sequence_data();
    return entropy(seq, seq_bytes);
}

void Mapper::set_alignment_threads(int new_thread_count) {
    alignment_threads = new_thread_count;
    clear_aligners(); // number of aligners per mapper depends on thread count
    init_aligner(alignment_match, alignment_mismatch, alignment_gap_open, alignment_gap_extension);
    init_node_cache();
    init_node_start_cache();
    init_node_pos_cache();
    init_edge_cache();
}

void Mapper::init_node_cache(void) {
    for (auto& nc : node_cache) {
        delete nc;
    }
    node_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        node_cache.push_back(new LRUCache<id_t, Node>(cache_size));
    }
}

void Mapper::init_node_start_cache(void) {
    for (auto& nc : node_start_cache) {
        delete nc;
    }
    node_start_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        node_start_cache.push_back(new LRUCache<id_t, size_t>(cache_size));
    }
}

void Mapper::init_node_pos_cache(void) {
    for (auto& nc : node_pos_cache) {
        delete nc;
    }
    node_pos_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        node_pos_cache.push_back(new LRUCache<gcsa::node_type, map<string, vector<size_t> > >(cache_size));
    }
}

void Mapper::init_edge_cache(void) {
    for (auto& ec : edge_cache) {
        delete ec;
    }
    edge_cache.clear();
    for (int i = 0; i < alignment_threads; ++i) {
        edge_cache.push_back(new LRUCache<id_t, vector<Edge> >(cache_size));
    }
}

void Mapper::clear_aligners(void) {
    for (auto& aligner : qual_adj_aligners) {
        delete aligner;
    }
    qual_adj_aligners.clear();
    for (auto& aligner : regular_aligners) {
        delete aligner;
    }
    regular_aligners.clear();
}

void Mapper::init_aligner(int8_t match, int8_t mismatch, int8_t gap_open, int8_t gap_extend) {
    // hacky, find max score so that scaling doesn't change score
    int8_t max_score = match;
    if (mismatch > max_score) max_score = mismatch;
    if (gap_open > max_score) max_score = gap_open;
    if (gap_extend > max_score) max_score = gap_extend;
    
    double gc_content = estimate_gc_content();

    for (int i = 0; i < alignment_threads; ++i) {
        qual_adj_aligners.push_back(new QualAdjAligner(match, mismatch, gap_open, gap_extend, max_score, 255, gc_content));
        regular_aligners.push_back(new Aligner(match, mismatch, gap_open, gap_extend));
    }
}

void Mapper::set_alignment_scores(int8_t match, int8_t mismatch, int8_t gap_open, int8_t gap_extend) {
    alignment_match = match;
    alignment_mismatch = mismatch;
    alignment_gap_open = gap_open;
    alignment_gap_extension = gap_extend;
    if (!qual_adj_aligners.empty()
        && !regular_aligners.empty()) {
        auto aligner = regular_aligners.front();
        // we've already set the right score
        if (match == aligner->match && mismatch == aligner->mismatch &&
            gap_open == aligner->gap_open && gap_extend == aligner->gap_extension) {
            return;
        }
        // otherwise, destroy them and reset
        clear_aligners();
    }
    // reset the aligners
    init_aligner(match, mismatch, gap_open, gap_extend);
}

// todo add options for aligned global and pinned
Alignment Mapper::align_to_graph(const Alignment& aln,
                                 VG& vg,
                                 size_t max_query_graph_ratio,
                                 bool pinned_alignment,
                                 bool pin_left,
                                 int8_t full_length_bonus,
                                 bool banded_global) {
    // check if we have a cached aligner for this thread
    if (aln.quality().empty() || !adjust_alignments_for_base_quality) {
        Aligner* aligner = get_regular_aligner();
        //aligner.align_global_banded(aln, graph.graph, band_padding);
        return vg.align(aln,
                        aligner,
                        max_query_graph_ratio,
                        pinned_alignment,
                        pin_left,
                        full_length_bonus,
                        banded_global,
                        0, // band padding override
                        aln.sequence().size());
    } else {
        QualAdjAligner* aligner = get_qual_adj_aligner();
        return vg.align_qual_adjusted(aln,
                                      aligner,
                                      max_query_graph_ratio,
                                      pinned_alignment,
                                      pin_left,
                                      full_length_bonus,
                                      banded_global,
                                      0, // band padding override
                                      aln.sequence().size());
    }
}

Alignment Mapper::align(const string& seq, int kmer_size, int stride, int max_mem_length, int band_width) {
    Alignment aln;
    aln.set_sequence(seq);
    return align(aln, kmer_size, stride, max_mem_length, band_width);
}

// align read2 near read1's mapping location
void Mapper::align_mate_in_window(const Alignment& read1, Alignment& read2, int pair_window) {
    if (read1.score() == 0) return; // bail out if we haven't aligned the first
    // try to recover in region
    auto& path = read1.path();
    int64_t idf = path.mapping(0).position().node_id();
    int64_t idl = path.mapping(path.mapping_size()-1).position().node_id();
    if(idf > idl) {
        swap(idf, idl);
    }
    // but which way should we expand? this will make things much easier.
    
    // We'll look near the leftmost and rightmost nodes, but we won't try and
    // bridge the whole area of the read, because there may be an ID
    // discontinuity.
    int64_t first = max((int64_t)0, idf - pair_window);
    int64_t last = idl + (int64_t) pair_window;
    
    // Now make sure the ranges don't overlap, because if they do we'll
    // duplicate nodes.
    
    // They can only overlap as idf on top of idl, since we swapped them above.
    // TODO: account at all for orientation? Maybe our left end is in higher
    // numbers but really does need to look left and not right.
    if(idf >= idl) {
        idf--;
    }
    
    VG* graph = new VG;

    if(debug) {
        cerr << "Rescuing in " << first << "-" << idf << " and " << idl << "-" << last << endl;
    }
    
    // TODO: how do we account for orientation when using ID ranges?

    // Now we need to get the neighborhood by ID and expand outward by actual
    // edges. How we do this depends on what indexing structures we have.
    if(xindex) {
        // should have callback here
        xindex->get_id_range(first, idf, graph->graph);
        xindex->get_id_range(idl, last, graph->graph);
        
        // don't get the paths (this isn't yet threadsafe in sdsl-lite)
        xindex->expand_context(graph->graph, context_depth, false);
        graph->rebuild_indexes();
    } else if(index) {
        index->get_range(first, idf, *graph);
        index->get_range(idl, last, *graph);
        index->expand_context(*graph, context_depth);
    } else {
        cerr << "error:[vg::Mapper] cannot align mate with no graph data" << endl;
        exit(1);
    }


    graph->remove_orphan_edges();
    
    if(debug) {
        cerr << "Rescue graph size: " << graph->size() << endl;
    }
    
    read2.clear_path();
    read2.set_score(0);

    read2 = align_to_graph(read2, *graph, max_query_graph_ratio);
    delete graph;
}

map<string, double> Mapper::alignment_mean_path_positions(const Alignment& aln, bool first_hit_only) {
    map<string, double> mean_pos;
    // Alignments are consistent if their median node id positions are within the fragment_size
    
    // We need the sets of nodes visited by each alignment
    set<id_t> ids;
    
    for(size_t i = 0; i < aln.path().mapping_size(); i++) {
        // Collect all the unique nodes visited by the first algnment
        ids.insert(aln.path().mapping(i).position().node_id());
    }
    map<string, map<int, vector<id_t> > > node_positions;
    for(auto id : ids) {
        for (auto& ref : node_positions_in_paths(gcsa::Node::encode(id, 0))) {
            auto& name = ref.first;
            for (auto pos : ref.second) {
                node_positions[name][pos].push_back(id);
            }
        }
        // just get the first one
        if (first_hit_only && node_positions.size()) break;
    }
    // get median mapping positions
    int idscount = 0;
    double idssum = 0;
    for (auto& ref : node_positions) {
        for (auto& p : ref.second) {
            for (auto& n : p.second) {
                auto pos = p.first + get_node_length(n)/2;
                if (ids.count(n)) {
                    idscount++;
                    idssum += pos;
                }
            }
        }
        mean_pos[ref.first] = idssum/idscount;
    }
    return mean_pos;
}

pos_t Mapper::likely_mate_position(const Alignment& aln, bool is_first_mate) {
    bool aln_is_rev = aln.path().mapping(0).position().is_reverse();
    int aln_pos = approx_alignment_position(aln);
    bool same_orientation = cached_fragment_orientation;
    bool forward_direction = cached_fragment_direction;
    int delta = cached_fragment_length_mean;
    // which way is our delta?
    // we are on the forward strand
    id_t target;
    if (forward_direction) {
        if (is_first_mate) {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos + delta);
            } else {
                target = node_approximately_at(aln_pos - delta);
            }
        } else {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos + delta);
            } else {
                target = node_approximately_at(aln_pos - delta);
            }
        }
    } else {
        if (is_first_mate) {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos - delta);
            } else {
                target = node_approximately_at(aln_pos + delta);
            }
        } else {
            if (!aln_is_rev) {
                target = node_approximately_at(aln_pos - delta);
            } else {
                target = node_approximately_at(aln_pos + delta);
            }
        }
    }
    if (same_orientation) {
        return make_pos_t(target, aln_is_rev, 0);
    } else {
        return make_pos_t(target, !aln_is_rev, 0);
    }
    /*
        && !aln_is_rev) {
    } else if (!same_direction && aln_is_rev) {
        target = (is_first_mate ? node_approximately_at(aln_pos + delta)
                  : node_approximately_at(aln_pos - delta));
    } else if (same_direction && aln_is_rev
               || !same_direction && !aln_is_rev) {
        target = (is_first_mate ? node_approximately_at(aln_pos - delta)
                  : node_approximately_at(aln_pos + delta));
    }
    */
    //bool target_is_rev = (same_orientation ? aln_is_rev : !aln_is_rev);
    //return make_pos_t(target, target_is_rev, 0);
}

bool Mapper::pair_rescue(Alignment& mate1, Alignment& mate2) {
    // bail out if we can't figure out how far to go
    if (!fragment_size) return false;
    //auto aligner = (mate1.quality().empty() ? get_regular_aligner() : get_qual_adj_aligner());
    double hang_threshold = 0.9;
    double retry_threshold = 0.7;
    //double hang_threshold = mate1.sequence().size() * aligner->match * 0.9;
    //double retry_threshold = mate1.sequence().size() * aligner->match * 0.3;
    //cerr << "hang " << hang_threshold << " retry " << retry_threshold << endl;
    //cerr << mate1.score() << " " << mate2.score() << endl;
    // based on our statistics about the alignments
    // get the subgraph overlapping the likely candidate position of the second alignment
    bool rescue_off_first = false;
    bool rescue_off_second = false;
    pos_t mate_pos;
    if (mate1.identity() > mate2.identity()
        && mate1.identity() >= hang_threshold
        && mate2.identity() < retry_threshold) {
        // retry off mate1
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "Rescue read 2 off of read 1" << endl;
        }
#endif
        rescue_off_first = true;
        // record id and direction to second mate
        mate_pos = likely_mate_position(mate1, true);
    } else if (mate2.identity() > mate1.identity()
               && mate2.identity() >= hang_threshold
               && mate1.identity() < retry_threshold) {
        // retry off mate2
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "Rescue read 1 off of read 2" << endl;
        }
#endif
        rescue_off_second = true;
        // record id and direction to second mate
        mate_pos = likely_mate_position(mate2, false);
    } else {
        return false;
    }
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "aiming for " << mate_pos << endl;
    }
#endif
    auto& node_cache = get_node_cache();
    auto& edge_cache = get_edge_cache();
    VG graph;
    int get_at_least = (!cached_fragment_length_mean ? fragment_max
                        : max((int)cached_fragment_length_stdev * 6 + mate1.sequence().size(),
                              mate1.sequence().size() * 4));
    cached_graph_context(graph, mate_pos, get_at_least/2, node_cache, edge_cache);
    cached_graph_context(graph, reverse(mate_pos, get_node_length(id(mate_pos))), get_at_least/2, node_cache, edge_cache);
    graph.remove_orphan_edges();
    //cerr << "got graph " << pb2json(graph.graph) << endl;
    // if we're reversed, align the reverse sequence and flip it back
    // align against it
    if (rescue_off_first) {
        Alignment aln2;
        bool flip = !mate1.path().mapping(0).position().is_reverse() && !cached_fragment_orientation
            || mate1.path().mapping(0).position().is_reverse() && cached_fragment_orientation;
        // do we expect the alignment to be on the reverse strand?
        if (flip) {
            aln2.set_sequence(reverse_complement(mate2.sequence()));
            if (!mate2.quality().empty()) {
                aln2.set_quality(mate2.quality());
                reverse(aln2.mutable_quality()->begin(),
                        aln2.mutable_quality()->end());
            }
        } else {
            aln2.set_sequence(mate2.sequence());
            if (!mate2.quality().empty()) {
                aln2.set_quality(mate2.quality());
            }
        }
        bool banded_global = false;
        bool pinned_alignment = false;
        bool pinned_reverse = false;
        aln2 = align_to_graph(aln2,
                              graph,
                              max_query_graph_ratio,
                              pinned_alignment,
                              pinned_reverse,
                              full_length_alignment_bonus,
                              banded_global);
        aln2.set_score(score_alignment(aln2));
        if (flip) {
            aln2 = reverse_complement_alignment(
                aln2,
                (function<int64_t(int64_t)>) ([&](int64_t id) {
                        return (int64_t)graph.get_node(id)->sequence().size();
                    }));
        }
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "aln2 score/ident vs " << aln2.score() << "/" << aln2.identity()
                            << " vs " << mate2.score() << "/" << mate2.identity() << endl;
        }
#endif
        if (aln2.score() > mate2.score()) {
            mate2 = aln2;
        } else {
            return false;
        }
    } else if (rescue_off_second) {
        Alignment aln1;
        bool flip = !mate2.path().mapping(0).position().is_reverse() && !cached_fragment_orientation
            || mate2.path().mapping(0).position().is_reverse() && cached_fragment_orientation;
        if (flip) {
            aln1.set_sequence(reverse_complement(mate1.sequence()));
            if (!mate1.quality().empty()) {
                aln1.set_quality(mate1.quality());
                reverse(aln1.mutable_quality()->begin(),
                        aln1.mutable_quality()->end());
            }
        } else {
            aln1.set_sequence(mate1.sequence());
            if (!mate1.quality().empty()) {
                aln1.set_quality(mate1.quality());
            }
        }
        bool banded_global = false;
        bool pinned_alignment = false;
        bool pinned_reverse = false;
        aln1 = align_to_graph(aln1,
                              graph,
                              max_query_graph_ratio,
                              pinned_alignment,
                              pinned_reverse,
                              full_length_alignment_bonus,
                              banded_global);
        //cerr << "score was " << aln1.score() << endl;
        aln1.set_score(score_alignment(aln1));
        //cerr << "score became " << aln1.score() << endl;
        if (flip) {
            aln1 = reverse_complement_alignment(
                aln1,
                (function<int64_t(int64_t)>) ([&](int64_t id) {
                        return (int64_t)graph.get_node(id)->sequence().size();
                    }));
        }
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "aln1 score/ident vs " << aln1.score() << "/" << aln1.identity()
                            << " vs " << mate1.score() << "/" << mate1.identity() << endl;
        }
#endif
        if (aln1.score() > mate1.score()) {
            mate1 = aln1;
        } else {
            return false;
        }
    }
    // if the new alignment is better
    // set the old alignment to it
    return true;
}

bool Mapper::alignments_consistent(const map<string, double>& pos1,
                                   const map<string, double>& pos2,
                                   int fragment_size_bound) {
    set<string> comm_refs;
    for (auto& p : pos1) {
        auto& name = p.first;
        if (pos2.find(name) != pos2.end()) {
            comm_refs.insert(name);
        }
    }
    // Alignments are consistent if their median node id positions are within the fragment_size
    
    // get median mapping positions
    for (auto& ref : comm_refs) {
        // this is unsafe looking, but we know they have the same keys for these values
        auto mean1 = pos1.find(ref)->second;
        auto mean2 = pos2.find(ref)->second;
        if (abs(mean1 - mean2) < fragment_size_bound) {
            return true;
        }
    }
    return false;
}

bool Mapper::pair_consistent(const Alignment& aln1,
                             const Alignment& aln2) {
    if (!(aln1.score() && aln2.score())) return false;
    bool length_ok = false;
    if (aln1.fragment_size() == 0) {
        // use the approximate distance
        int len = approx_fragment_length(aln1, aln2);
        if (len > 0 && len < fragment_size
            || !fragment_size && len > 0 && len < fragment_max) {
            length_ok = true;
        }
    } else {
        // use the distance induced by the graph paths
        assert(aln1.fragment_size() == aln2.fragment_size());
        for (size_t i = 0; i < aln1.fragment_size(); ++i) {
            int len = abs(aln1.fragment(i).length());
            if (len > 0 && len < fragment_size
                || !fragment_size && len > 0 && len < fragment_max) {
                length_ok = true;
                break;
            }
        }
    }
    bool aln1_is_rev = aln1.path().mapping(0).position().is_reverse();
    bool aln2_is_rev = aln1.path().mapping(0).position().is_reverse();
    bool same_orientation = cached_fragment_orientation;
    bool orientation_ok = same_orientation && aln1_is_rev == aln2_is_rev
        || !same_orientation && aln1_is_rev != aln2_is_rev;
    return length_ok && orientation_ok;
}

pair<vector<Alignment>, vector<Alignment>> Mapper::align_paired_multi(
    const Alignment& read1,
    const Alignment& read2,
    bool& queued_resolve_later,
    int max_mem_length,
    bool only_top_scoring_pair,
    bool retrying) {

    double avg_node_len = average_node_length();
    int8_t match;
    int8_t gap_extension;
    int8_t gap_open;
    if (read1.quality().empty() || !adjust_alignments_for_base_quality) {
        auto aligner =  get_regular_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    else {
        auto aligner =  get_qual_adj_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    int total_multimaps = max_multimaps + extra_multimaps;
    double cluster_mq = 0;

    if(debug) {
        cerr << "align_paired_multi_simul "
             << "with " << read1.name() << " and "
             << read2.name() << endl
            //<< "read 1 " << read1.sequence() << endl
            //<< "read 2 " << read2.sequence() << endl
            //<< "read 1 " << pb2json(read1) << endl
            //<< "read 2 " << pb2json(read2) << endl
             << "fragment model " << fragment_max << ", "
             << fragment_size << ", "
             << cached_fragment_length_mean << ", "
             << cached_fragment_length_stdev << ", "
             << cached_fragment_orientation << ", "
             << cached_fragment_direction << ", "
             << since_last_fragment_length_estimate << ", " << endl;
    }

    pair<vector<Alignment>, vector<Alignment>> results;
    double longest_lcp1, longest_lcp2;
    // find the MEMs for the alignments
    vector<MaximalExactMatch> mems1 = find_mems_deep(read1.sequence().begin(),
                                                     read1.sequence().end(),
                                                     longest_lcp1,
                                                     max_mem_length,
                                                     min_mem_length,
                                                     mem_reseed_length);
    vector<MaximalExactMatch> mems2 = find_mems_deep(read2.sequence().begin(),
                                                     read2.sequence().end(),
                                                     longest_lcp2,
                                                     max_mem_length,
                                                     min_mem_length,
                                                     mem_reseed_length);

    double mq_cap1, mq_cap2;
    mq_cap1 = mq_cap2 = max_mapping_quality;

    {
        int mem_max_length1 = 0;
        for (auto& mem : mems1) if (mem.primary && mem.match_count) mem_max_length1 = max(mem_max_length1, (int)mem.length());
        double maybe_max1 = estimate_max_possible_mapping_quality(read1.sequence().size(),
                                                                  read1.sequence().size()/max(1.0, (double)mem_max_length1),
                                                                  read1.sequence().size()/longest_lcp1);
        int mem_max_length2 = 0;
        for (auto& mem : mems2) if (mem.primary && mem.match_count) mem_max_length2 = max(mem_max_length2, (int)mem.length());
        double maybe_max2 = estimate_max_possible_mapping_quality(read2.sequence().size(),
                                                                  read2.sequence().size()/max(1.0, (double)mem_max_length2),
                                                                  read2.sequence().size()/longest_lcp2);
        // use the estimated mapping quality to avoid hard work when the results are likely noninformative
        double maybe_min = min(maybe_max1, maybe_max2);
        if (maybe_min < maybe_mq_threshold) {
            mq_cap1 = maybe_min;
            mq_cap2 = maybe_min;
        }
        if (min_multimaps < max_multimaps) {
            total_multimaps = max(min_multimaps, (int)round(total_multimaps/min(maybe_max1,maybe_max2)));
        }
    }

//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "mems for read 1 " << mems_to_json(mems1) << endl;
    }
//#endif
//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "mems for read 2 " << mems_to_json(mems2) << endl;
    }
//#endif


    auto transition_weight = [&](const MaximalExactMatch& m1, const MaximalExactMatch& m2) {

        // set up positions for distance query
        pos_t m1_pos = make_pos_t(m1.nodes.front());
        pos_t m2_pos = make_pos_t(m2.nodes.front());
        //double uniqueness = 2.0 / (m1.match_count + m2.match_count);

        // approximate distance by node lengths
        int approx_dist = approx_distance(m1_pos, m2_pos);

        // are the two mems in a different fragment?
        // we handle the distance metric differently in these cases
        if (m1.fragment < m2.fragment) {
            int max_length = fragment_max;
            int dist = abs(approx_dist);
            int unique_coverage = m1.length() + m2.length();
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "between fragment approx distance " << approx_dist << endl;
            }
#endif
            if (dist >= max_length) {
                return -std::numeric_limits<double>::max();
            } else {
                if (xindex->path_count) {
                    dist = xindex->min_approx_path_distance({}, id(m1_pos), id(m2_pos));
                }
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "---> true distance from " << m1_pos << " to " << m2_pos << " = "<< dist << endl;
                }
#endif
                if (dist >= max_length) {
                    return -std::numeric_limits<double>::max();
                } else if (fragment_size) {
                    // exclude cases that don't match our model
                    if (!cached_fragment_orientation
                        && is_rev(m1_pos) == is_rev(m2_pos)
                        || cached_fragment_orientation
                        && is_rev(m1_pos) != is_rev(m2_pos)
                        || dist > fragment_size) {
                        return -std::numeric_limits<double>::max();
                    } else {
                        return fragment_length_pdf(dist)/fragment_length_pdf(cached_fragment_length_mean);// * unique_coverage * uniqueness;
                    }
                } else {
                    return 1.0/dist;// * unique_coverage * uniqueness;
                }
            }
        } else if (m1.fragment > m2.fragment) {
            // don't allow going backwards in the threads
            return -std::numeric_limits<double>::max();
        } else {
            //int max_length = (m1.length() + m2.length());
            int max_length = max(read1.sequence().size(), read2.sequence().size());
            // find the difference in m1.end and m2.begin
            // find the positional difference in the graph between m1.end and m2.begin
            int unique_coverage = (m1.length() + m2.length()) - mems_overlap_length(m1, m2);
            approx_dist = abs(approx_dist);
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "in fragment approx distance " << approx_dist << endl;
            }
#endif
            if (approx_dist > max_length) {
                // too far
                return -std::numeric_limits<double>::max();
            } else {
                // we may want to switch back to exact measurement, although the approximate metric is simpler and more reliable despite being less precise
                // int distance = graph_distance(m1_pos, m2_pos, max_length); // enable for exact distance calculation
                int distance = approx_dist;
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "---> true distance " << distance << endl;
                }
#endif
                if (distance == max_length) {
                    return -std::numeric_limits<double>::max();
                }
                if (is_rev(m1_pos) != is_rev(m2_pos)) {
                    // disable inversions
                    return -std::numeric_limits<double>::max();
                } else {
                    // accepted transition
                    double jump = abs((m2.begin - m1.begin) - distance);
                    if (jump) {
                        return (double) unique_coverage * match - (gap_open + jump * gap_extension);
                    } else {
                        return (double) unique_coverage * match;
                    }
                }
            }
        }
    };

    // build the paired-read MEM markov model
    vector<vector<MaximalExactMatch> > clusters;
    if (total_multimaps) {
        MEMChainModel chainer({ read1.sequence().size(), read2.sequence().size() },
                              { mems1, mems2 }, this,
                              transition_weight,
                              max((int)(read1.sequence().size() + read2.sequence().size()),
                                  (int)(fragment_size ? fragment_size : fragment_max)));
        clusters = chainer.traceback(total_multimaps, true, debug);
    }

    // don't attempt to align if we reach the maximum number of multimaps
    //if (clusters.size() == total_multimaps) clusters.clear();
    // disabled as this seems to cause serious problems at low cluster sizes

    auto show_clusters = [&](void) {
        cerr << "clusters: " << endl;
        for (auto& cluster : clusters) {
            cerr << cluster.size() << " MEMs covering " << cluster_coverage(cluster) << " @ ";
            for (auto& mem : cluster) {
                size_t len = mem.begin - mem.end;
                for (auto& node : mem.nodes) {
                    id_t id = gcsa::Node::id(node);
                    size_t offset = gcsa::Node::offset(node);
                    bool is_rev = gcsa::Node::rc(node);
                    cerr << "|" << id << (is_rev ? "-" : "+") << ":" << offset << "," << mem.fragment << ",";
                }
                cerr << mem.sequence() << " ";
            }
            cerr << endl;
        }
    };

#pragma omp critical
    {
        if (debug) {
            cerr << "### clusters:" << endl;
            show_clusters();
        }
    }

    auto to_drop = clusters_to_drop(clusters);
    vector<pair<Alignment, Alignment> > alns;
    //pair<vector<Alignment>, vector<Alignment> > alns;
    int multimaps = 0;
    for (auto& cluster : clusters) {
        // skip if we've filtered the cluster
        if (to_drop.count(&cluster) && multimaps >= min_multimaps) continue;
        // stop if we have enough multimaps
        if (multimaps > total_multimaps) { break; }
        // skip if we've got enough multimaps to get MQ and we're under the min cluster length
        if (min_cluster_length && cluster_coverage(cluster) < min_cluster_length && alns.size() > 1) continue;
        // break the cluster into two pieces
        vector<MaximalExactMatch> cluster1, cluster2;
        bool seen1=false, seen2=false;
        for (auto& mem : cluster) {
            if (!seen2 && mem.fragment == 1) {
                cluster1.push_back(mem);
                seen1 = true;
            } else if (mem.fragment == 2) {
                cluster2.push_back(mem);
                seen2 = true;
            } else {
                cerr << "vg map error misordered fragments in cluster" << endl;
                assert(false);
            }
        }
        alns.emplace_back();
        auto& p = alns.back();
        if (cluster1.size()) {
            p.first = align_cluster(read1, cluster1);
        } else {
            p.first = read1;
            p.first.clear_score();
            p.first.clear_identity();
            p.first.clear_path();
        }
        if (cluster2.size()) {
            p.second = align_cluster(read2, cluster2);
        } else {
            p.second = read2;
            p.second.clear_score();
            p.second.clear_identity();
            p.second.clear_path();
        }

        ++multimaps;

    }

    auto sort_and_dedup = [&](void) {
        // sort the aligned pairs
        std::sort(alns.begin(), alns.end(),
                  [&](const pair<Alignment, Alignment>& pair1,
                      const pair<Alignment, Alignment>& pair2) {
                      double bonus1=0, bonus2=0;
                      if (fragment_size) {
                          int dist1 = approx_fragment_length(pair1.first, pair1.second);
                          int dist2 = approx_fragment_length(pair2.first, pair2.second);
                          if (dist1 >= 0 && pair_consistent(pair1.first, pair1.second)) {
                              bonus1 = fragment_length_pdf(dist1) * cached_fragment_length_mean;
                          }
                          if (dist2 >= 0 && pair_consistent(pair2.first, pair2.second)) {
                              bonus2 = fragment_length_pdf(dist2) * cached_fragment_length_mean;
                          }
                      }
                      return (pair1.first.score() + pair1.second.score()) + bonus1
                      > (pair2.first.score() + pair2.second.score()) + bonus2;
                  });
        // remove duplicates (same score and same start position of both pairs)
        alns.erase(
            std::unique(
                alns.begin(), alns.end(),
                [&](const pair<Alignment, Alignment>& pair1,
                    const pair<Alignment, Alignment>& pair2) {
                    bool same = true;
                    if (pair1.first.score() && pair2.first.score()) {
                        same &= make_pos_t(pair1.first.path().mapping(0).position())
                            == make_pos_t(pair2.first.path().mapping(0).position());
                    }
                    if (pair1.second.score() && pair2.second.score()) {
                        same &= make_pos_t(pair1.second.path().mapping(0).position())
                            == make_pos_t(pair2.second.path().mapping(0).position());
                    }
                    if (!(pair1.first.score() && pair2.first.score()
                          || pair1.second.score() && pair2.second.score())) {
                        same = false;
                    }
                    return same;
                }),
            alns.end());
    };
    sort_and_dedup();
    if (fragment_size) {
        // go through the pairs and see if we need to rescue one side off the other
        bool rescued = false;
        int j = 0;
        for (auto& p : alns) {
            rescued |= pair_rescue(p.first, p.second);
            if (++j == mate_rescues) break;
        }
        if (rescued) sort_and_dedup();
    }

#pragma omp critical
    {
        if (debug) {
            for (auto& p : alns) {
                auto& aln1 = p.first;
                cerr << "1:" << aln1.score();
                if (aln1.score()) cerr << "@" << aln1.path().mapping(0).position().node_id() << " ";
                //cerr << endl;
                //cerr << "cluster aln 1 ------- " << pb2json(aln1) << endl;
                if (!check_alignment(aln1)) {
                    cerr << "alignment failure " << pb2json(aln1) << endl;
                    assert(false);
                }
                auto& aln2 = p.second;
                cerr << "2:" << aln2.score();
                if (aln2.score()) cerr << "@" << aln2.path().mapping(0).position().node_id() << " ";
                //cerr << endl;
                //cerr << "cluster aln 2 ------- " << pb2json(aln2) << endl;
                if (!check_alignment(aln2)) {
                    cerr << "alignment failure " << pb2json(aln2) << endl;
                    assert(false);
                }
                cerr << endl;
            }
        }
    }

    // calculate cluster mapping quality
    if (use_cluster_mq) {
        cluster_mq = compute_cluster_mapping_quality(clusters, read1.sequence().size() + read2.sequence().size());
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "cluster mq == " << cluster_mq << endl;
        }
#endif
    }

    // rebuild the thing we'll return
    int read1_max_score = 0;
    int read2_max_score = 0;
    for (auto& p : alns) {
        read1_max_score = max(p.first.score(), read1_max_score);
        read2_max_score = max(p.second.score(), read2_max_score);
        results.first.push_back(p.first);
        results.second.push_back(p.second);
    }
    compute_mapping_qualities(results, cluster_mq, mq_cap1, mq_cap2, max_mapping_quality, max_mapping_quality);

    // remove the extra pair used to compute mapping quality if necessary
    if (results.first.size() > max_multimaps) {
        results.first.resize(max_multimaps);
        results.second.resize(max_multimaps);
    }

    // mark primary and secondary
    for (int i = 0; i < results.first.size(); i++) {
        results.first[i].mutable_fragment_next()->set_name(read2.name());
        results.first[i].set_is_secondary(i > 0);
        results.second[i].mutable_fragment_prev()->set_name(read1.name());
        results.second[i].set_is_secondary(i > 0);
    }

    // optionally zap everything unless the primary alignments are individually top-scoring
    if (only_top_scoring_pair && results.first.size() &&
        (results.first[0].score() < read1_max_score ||
         results.second[0].score() < read2_max_score)) {
        results.first.clear();
        results.second.clear();
    }
    
    // we tried to align
    // if we don't have a fragment_size yet determined
    // and we didn't get a perfect, unambiguous hit on both reads
    // we'll need to try it again later when we do have a fragment_size
    // so store it in a buffer local to this mapper

    // tag the results with their fragment lengths
    // record the lengths in a deque that we use to keep a running estimate of the fragment length distribution
    // we then set the fragment_size cutoff using the moments of the estimated distribution
    bool imperfect_pair = false;
    for (int i = 0; i < min(results.first.size(), results.second.size()); ++i) {
        if (retrying) break;
        auto& aln1 = results.first.at(i);
        auto& aln2 = results.second.at(i);
        auto approx_frag_lengths = approx_pair_fragment_length(aln1, aln2);
        for (auto& j : approx_frag_lengths) {
            Path fragment;
            fragment.set_name(j.first);
            fragment.set_length(j.second);
            *aln1.add_fragment() = fragment;
            *aln2.add_fragment() = fragment;
            // if we have a perfect mapping, and we're under our hard fragment length cutoff
            // push the result into our deque of fragment lengths
            if (results.first.size() == 1
                && results.second.size() == 1
                && results.first.front().identity() > perfect_pair_identity_threshold
                && results.second.front().identity() > perfect_pair_identity_threshold
                && (fragment_size && abs(j.second) < fragment_size
                    || !fragment_size && abs(j.second) < fragment_max)) { // hard cutoff
                //cerr << "aln\tperfect alignments" << endl;
                record_fragment_configuration(j.second, aln1, aln2);
            } else if (!fragment_size) {
                imperfect_pair = true;
                break;
            }
        }
    }

    if (!retrying && imperfect_pair && fragment_max) {
        imperfect_pairs_to_retry.push_back(make_pair(read1, read2));
        results.first.clear();
        results.second.clear();
        // we signal the fact that this isn't a perfect pair, so we don't write it out externally?
        queued_resolve_later = true;
    }

    if(results.first.empty()) {
        results.first.push_back(read1);
        auto& aln = results.first.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }
    if(results.second.empty()) {
        results.second.push_back(read2);
        auto& aln = results.second.back();
        aln.clear_path();
        aln.clear_score();
        aln.clear_identity();
    }

    // Make sure to link up alignments even if they aren't mapped.
    for (auto& aln : results.first) {
        aln.set_name(read1.name());
        aln.mutable_fragment_next()->set_name(read2.name());
    }

    for (auto& aln : results.second) {
        aln.set_name(read2.name());
        aln.mutable_fragment_prev()->set_name(read1.name());
    }

    return results;

}

// rank the clusters by the number of unique read bases they cover
int cluster_coverage(const vector<MaximalExactMatch>& cluster) {
    set<string::const_iterator> seen;
    for (auto& mem : cluster) {
        string::const_iterator c = mem.begin;
        while (c != mem.end) seen.insert(c++);
    }
    return seen.size();
}

double Mapper::compute_cluster_mapping_quality(const vector<vector<MaximalExactMatch> >& clusters,
                                               int read_length) {
    if (clusters.size() == 0) {
        return 0;
    }
    if (clusters.size() == 1) {
        return { (double)max_cluster_mapping_quality };
    }
    vector<double> weights;
    for (auto& cluster : clusters) {
        weights.emplace_back();
        double& weight = weights.back();
        for (int i = 0; i < cluster.size(); ++i) {
            // for each mem, count half of its coverage with its neighbors towards this metric
            auto& mem = cluster[i];
            int shared_coverage = 0;
            if (i > 0) {
                auto& prev = cluster[i-1];
                if (prev.fragment == mem.fragment) {
                    shared_coverage += (prev.end <= mem.begin ? 0 : prev.end - mem.begin);
                }
            }
            if (i < cluster.size()-1) {
                auto& next = cluster[i+1];
                if (next.fragment == mem.fragment) {
                    shared_coverage += (mem.end <= next.begin ? 0 : mem.end - next.begin);
                }
            }
            weight +=
                (((double)mem.length() - (double)shared_coverage/2)
                 / read_length)
                / mem.match_count;
        }
        //cerr << "weight " << weight << endl;
    }
    // return the ratio between best and second best as quality
    std::sort(weights.begin(), weights.end(), std::greater<double>());
    // find how many maxes we have
    double max_weight = weights.front();
    int max_count = 0;
    while (max_weight == weights[max_count]) ++max_count;
    double best_chance = max_count > 1 ? prob_to_phred(1.0-(1.0/max_count)) : 0;
    if (weights[0] == 0) return 0;
    return min((double)max_cluster_mapping_quality,
               max(best_chance, prob_to_phred(weights[1]/weights[0])));
}

double
Mapper::average_node_length(void) {
    return (double) xindex->seq_length / (double) xindex->node_count;
}

bool mems_overlap(const MaximalExactMatch& mem1,
                  const MaximalExactMatch& mem2) {
    // we overlap if we are not completely separated
    return mem1.fragment == mem2.fragment
        && !(mem1.end <= mem2.begin
             || mem2.end <= mem1.begin);
}

int mems_overlap_length(const MaximalExactMatch& mem1,
                        const MaximalExactMatch& mem2) {
    if (!mems_overlap(mem1, mem2)) {
        return 0;
    } else {
        if (mem1.begin < mem2.begin) {
            if (mem1.end < mem2.end) {
                return mem1.end - mem2.begin;
            } else {
                return mem1.end - mem1.begin;
            }
        } else {
            if (mem2.end < mem1.end) {
                return mem2.end - mem1.begin;
            } else {
                return mem2.end - mem2.begin;
            }
        }
    }
}

bool clusters_overlap(const vector<MaximalExactMatch>& cluster1,
                      const vector<MaximalExactMatch>& cluster2) {
    for (auto& mem1 : cluster1) {
        for (auto& mem2 : cluster2) {
            if (mems_overlap(mem1, mem2)) {
                return true;
            }
        }
    }
    return false;
}

int sub_overlaps_of_first_aln(const vector<Alignment>& alns, float overlap_fraction) {
    // take the first
    // now look at the rest and measure overlap
    if (alns.empty()) return 0;
    auto& aln1 = alns.front();
    int seq_len = aln1.sequence().size();
    int overlaps = 0;
    for (int i = 1; i < alns.size(); ++i) {
        auto& aln2 = alns[i];
        // where the overlap is greater than overlap_fraction
        if ((double)query_overlap(aln1, aln2)/seq_len >= overlap_fraction) {
            ++overlaps;
        }
    }
    return overlaps;
}

set<const vector<MaximalExactMatch>* > Mapper::clusters_to_drop(const vector<vector<MaximalExactMatch> >& clusters) {
    set<const vector<MaximalExactMatch>* > to_drop;
    map<const vector<MaximalExactMatch>*, int> cluster_cov;
    for (auto& cluster : clusters) {
        cluster_cov[&cluster] = cluster_coverage(cluster);
    }
    for (int i = 0; i < clusters.size(); ++i) {
        // establish overlaps with longer clusters for all clusters
        auto& this_cluster = clusters[i];
        int t = cluster_cov[&this_cluster];
        int b = -1;
        int l = t;
        for (int j = i; j >= 0; --j) {
            if (j == i) continue;
            // are we overlapping?
            auto& other_cluster = clusters[j];
            if (clusters_overlap(this_cluster, other_cluster)) {
                int c = cluster_cov[&other_cluster];
                if (c > l) {
                    l = c;
                    b = j;
                }
            }
        }
        if (b >= 0 && (float) t / (float) l < drop_chain) {
            to_drop.insert(&this_cluster);
        }
    }
    return to_drop;
}

vector<Alignment>
Mapper::align_mem_multi(const Alignment& aln, vector<MaximalExactMatch>& mems, double& cluster_mq, double longest_lcp, int max_mem_length, int additional_multimaps) {

//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "mems for read " << mems_to_json(mems) << endl;
    }
//#endif
    
    int match;
    int gap_extension;
    int gap_open;
    if (aln.quality().empty() || !adjust_alignments_for_base_quality) {
        auto aligner =  get_regular_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }
    else {
        auto aligner =  get_qual_adj_aligner();
        match = aligner->match;
        gap_extension = aligner->gap_extension;
        gap_open = aligner->gap_open;
    }

    int total_multimaps = max_multimaps + additional_multimaps;
    double mq_cap = max_mapping_quality;

    {
        int mem_max_length = 0;
        for (auto& mem : mems) if (mem.primary && mem.match_count) mem_max_length = max(mem_max_length, (int)mem.length());
        double maybe_max = estimate_max_possible_mapping_quality(aln.sequence().size(),
                                                                 aln.sequence().size()/max(1.0, (double)mem_max_length),
                                                                 aln.sequence().size()/longest_lcp);
        // use the estimated mapping quality to avoid hard work when the outcome will be noninformative
        if (maybe_max < maybe_mq_threshold) {
            mq_cap = maybe_max;
        }
        if (min_multimaps < max_multimaps) {
            total_multimaps = max(min_multimaps, (int)round(total_multimaps/maybe_max));
        }
        if (debug) cerr << "maybe_mq " << aln.name() << " " << maybe_max << " " << total_multimaps << " " << mem_max_length << " " << longest_lcp << endl;
    }

    double avg_node_len = average_node_length();
    // go through the ordered single-hit MEMs
    // build the clustering model
    // find the alignments that are the best-scoring walks through it
    auto transition_weight = [&](const MaximalExactMatch& m1, const MaximalExactMatch& m2) {

        int unique_coverage = m1.length() + m2.length() - mems_overlap_length(m1, m2);
        pos_t m1_pos = make_pos_t(m1.nodes.front());
        pos_t m2_pos = make_pos_t(m2.nodes.front());
        int max_length = aln.sequence().size();
        int approx_dist = abs(approx_distance(m1_pos, m2_pos));
        
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "mems " << &m1 << ":" << m1 << " -> " << &m2 << ":" << m2 << " approx_dist " << approx_dist << endl;
        }
#endif
        if (approx_dist > max_length) {
            // too far
            return -std::numeric_limits<double>::max();
        } else {
            if (is_rev(m1_pos) != is_rev(m2_pos)) {
                // disable inversions
                return -std::numeric_limits<double>::max();
            } else {
                // accepted transition
                double jump = abs((m2.begin - m1.begin) - approx_dist);
                if (jump) {
                    return (double) unique_coverage * match - (gap_open + jump * gap_extension);
                } else {
                    return (double) unique_coverage * match;
                }
            }
        }
    };

    // establish the chains
    vector<vector<MaximalExactMatch> > clusters;
    if (total_multimaps) {
        MEMChainModel chainer({ aln.sequence().size() }, { mems }, this, transition_weight, aln.sequence().size());
        clusters = chainer.traceback(total_multimaps, false, debug);
    }

    // don't attempt to align if we reach the maximum number of multimaps
    //if (clusters.size() == total_multimaps) clusters.clear();

    auto show_clusters = [&](void) {
        cerr << "clusters: " << endl;
        for (auto& cluster : clusters) {
            cerr << cluster.size() << " MEMs covering " << cluster_coverage(cluster) << " @ ";
            for (auto& mem : cluster) {
                size_t len = mem.begin - mem.end;
                for (auto& node : mem.nodes) {
                    id_t id = gcsa::Node::id(node);
                    size_t offset = gcsa::Node::offset(node);
                    bool is_rev = gcsa::Node::rc(node);
                    cerr << "|" << id << (is_rev ? "-" : "+") << ":" << offset << "," << mem.fragment << ",";
                    /*
                    for (auto& ref : node_positions_in_paths(gcsa::Node::encode(id, 0, is_rev))) {
                        auto& name = ref.first;
                        for (auto pos : ref.second) {
                            //cerr << name << (is_rev?"-":"+") << pos + offset;
                            cerr << "|" << id << (is_rev ? "-" : "+") << ":" << offset << ",";
                        }
                    }
                    */
                }
                cerr << mem.sequence() << " ";
            }
            cerr << endl;
        }
    };

//#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "### clusters:" << endl;
            show_clusters();
        }
    }
//#endif

    if (use_cluster_mq) {
        cluster_mq = compute_cluster_mapping_quality(clusters, aln.sequence().size());
    }
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "cluster mapping quality " << cluster_mq << endl;
        }
    }
#endif
    auto to_drop = clusters_to_drop(clusters);

    // for up to our required number of multimaps
    // make the perfect-match alignment for the SMEM cluster
    // then fix it up with DP on the little bits between the alignments
    vector<Alignment> alns;
    int multimaps = 0;
    for (auto& cluster : clusters) {
        // filtered out due to overlap with longer chain
        if (to_drop.count(&cluster) && multimaps >= min_multimaps) continue;
        if (++multimaps > total_multimaps) { break; }
        // skip this if we don't have sufficient cluster coverage and we have at least two alignments
        // which we can use to estimate mapping quality
        if (min_cluster_length && cluster_coverage(cluster) < min_cluster_length && alns.size() > 1) continue;
        // get the candidate graph
        // align to it
        Alignment candidate = align_cluster(aln, cluster);
        if (candidate.identity() > min_identity) {
            alns.emplace_back(candidate);
        }
    }

#pragma omp critical
    if (debug) {
        cerr << "alignments" << endl;
        for (auto& aln : alns) {
            cerr << aln.score();
            if (aln.score()) cerr << " pos1 " << aln.path().mapping(0).position().node_id() << " ";
            cerr << endl;
        }
    }

#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            for (auto& aln : alns) {
                cerr << "cluster aln ------- " << pb2json(aln) << endl;
            }
            for (auto& aln : alns) {
                if (!check_alignment(aln)) {
                    cerr << "alignment failure " << pb2json(aln) << endl;
                    assert(false);
                }
            }
        }
    }
#endif
    // sort alignments by score
    // then by complexity (measured as number of edit operations)
    std::sort(alns.begin(), alns.end(),
              [&](const Alignment& a1, const Alignment& a2) {
                  return a1.score() > a2.score()
                      || a1.score() == a2.score()
                      && edit_count(a1) > edit_count(a2);
              });
    // remove likely perfect duplicates
    alns.erase(
        std::unique(
            alns.begin(), alns.end(),
            [&](const Alignment& aln1,
                const Alignment& aln2) {
                return
                    aln1.score() == aln2.score()
                    && (aln1.score() == 0
                        || make_pos_t(aln1.path().mapping(0).position())
                        == make_pos_t(aln2.path().mapping(0).position()));
            }),
        alns.end());
    // second round of sorting and deduplication
    alns = score_sort_and_deduplicate_alignments(alns, aln);
    // and finally, compute the mapping quality
    compute_mapping_qualities(alns, cluster_mq, mq_cap, max_mapping_quality);
    // prune to max_multimaps
    filter_and_process_multimaps(alns, 0);

    return alns;
}

Alignment Mapper::align_maybe_flip(const Alignment& base, VG& graph, bool flip) {
    Alignment aln = base;
    if (flip) {
        aln.set_sequence(reverse_complement(base.sequence()));
        if (!base.quality().empty()) {
            aln.set_quality(base.quality());
            reverse(aln.mutable_quality()->begin(),
                    aln.mutable_quality()->end());
        }
    } else {
        aln.set_sequence(base.sequence());
        if (!base.quality().empty()) {
            aln.set_quality(base.quality());
        }
    }
    bool banded_global = false;
    bool pinned_alignment = false;
    bool pinned_reverse = false;
    aln = align_to_graph(aln,
                         graph,
                         max_query_graph_ratio,
                         pinned_alignment,
                         pinned_reverse,
                         full_length_alignment_bonus,
                         banded_global);
    aln.set_score(rescore_without_full_length_bonus(aln));
    if (flip) {
        aln = reverse_complement_alignment(
            aln,
            (function<int64_t(int64_t)>) ([&](int64_t id) {
                    return (int64_t)graph.get_node(id)->sequence().size();
                }));
    }
    return aln;
}

double Mapper::compute_uniqueness(const Alignment& aln, const vector<MaximalExactMatch>& mems) {
    // compute the per-base copy number of the alignment based on the MEMs in the cluster
    vector<int> v; v.resize(aln.sequence().size());
    auto aln_begin = aln.sequence().begin();
    for (auto& mem : mems) {
        // from the beginning to the end of the mem in the read
        // add the count of hits to each position
        for (int i = mem.begin - aln_begin; i < mem.end - aln_begin; ++i) {
            v[i] += mem.match_count;
        }
    }
    // what is the fraction of the cluster that is in repetitive mems
    // we calculate the uniqueness metric as the average number of hits for bases in the read covered by any MEM
    double repeated = std::accumulate(v.begin(), v.end(), 0.0,
                                      [](const int& a, const int& b) { return b > 1 ? a + 1 : a; });
    return repeated / aln.sequence().length();
}

Alignment Mapper::align_cluster(const Alignment& aln, const vector<MaximalExactMatch>& mems) {
    // poll the mems to see if we should flip
    int count_fwd = 0, count_rev = 0;
    for (auto& mem : mems) {
        bool is_rev = gcsa::Node::rc(mem.nodes.front());
        if (is_rev) {
            ++count_rev;
        } else {
            ++count_fwd;
        }
    }
    // get the graph
    VG graph = cluster_subgraph(aln, mems);
    // and test each direction for which we have MEM hits
    Alignment aln_fwd;
    Alignment aln_rev;
    if (count_fwd) {
        aln_fwd = align_maybe_flip(aln, graph, false);
    }
    if (count_rev) {
        aln_rev = align_maybe_flip(aln, graph, true);
    }
    if (aln_fwd.score() + aln_rev.score() == 0) {
        // abject failure, nothing aligned with score > 0
        Alignment result = aln;
        result.clear_path();
        result.clear_score();
        return result;
    } else if (aln_rev.score() > aln_fwd.score()) {
        // reverse won
        return aln_rev;
    } else {
        // forward won
        return aln_fwd;
    }
}

void Mapper::cached_graph_context(VG& graph, const pos_t& pos, int length, LRUCache<id_t, Node>& node_cache, LRUCache<id_t, vector<Edge> >& edge_cache) {
    // walk the graph from this position forward
    // adding the nodes we run into to the graph
    set<pos_t> seen;
    set<pos_t> nexts;
    nexts.insert(pos);
    int distance = -offset(pos); // don't count what we won't traverse
    while (!nexts.empty()) {
        set<pos_t> todo;
        int nextd = 0;
        for (auto& next : nexts) {
            if (!seen.count(next)) {
                seen.insert(next);
                // add the node and its edges to the graph
                Node node = xg_cached_node(id(next), xindex, node_cache);
                nextd = nextd == 0 ? node.sequence().size() : min(nextd, (int)node.sequence().size());
                //distance += node.sequence().size();
                graph.add_node(node);
                for (auto& edge : xg_cached_edges_of(id(next), xindex, edge_cache)) {
                    graph.add_edge(edge);
                }
                // where to next
                for (auto& x : xg_cached_next_pos(next, true, xindex, node_cache, edge_cache)) {
                    todo.insert(x);
                }
            }
        }
        distance += nextd;
        if (distance > length) {
            break;
        }
        nexts = todo;
    }
    return;
}

VG Mapper::cluster_subgraph(const Alignment& aln, const vector<MaximalExactMatch>& mems) {
    auto& node_cache = get_node_cache();
    auto& edge_cache = get_edge_cache();
    assert(mems.size());
    auto& start_mem = mems.front();
    auto start_pos = make_pos_t(start_mem.nodes.front());
    auto rev_start_pos = reverse(start_pos, get_node_length(id(start_pos)));
    float expansion = 1.61803;
    int get_before = (int)(start_mem.begin - aln.sequence().begin()) * expansion;
    VG graph;
    if (get_before) {
        cached_graph_context(graph, rev_start_pos, get_before, node_cache, edge_cache);
    }
    for (int i = 0; i < mems.size(); ++i) {
        auto& mem = mems[i];
        auto pos = make_pos_t(mem.nodes.front());
        int get_after = expansion * (i+1 == mems.size() ? aln.sequence().end() - mem.begin
                                     : max(mem.length(), (int)(mems[i+1].begin - mem.begin)));
        //if (debug) cerr << pos << " " << mem << " getting after " << get_after << endl;
        cached_graph_context(graph, pos, get_after, node_cache, edge_cache);
    }
    graph.remove_orphan_edges();
    //if (debug) cerr << "graph " << pb2json(graph.graph) << endl;
    return graph;
}

VG Mapper::alignment_subgraph(const Alignment& aln, int context_size) {
    set<id_t> nodes;
    auto& path = aln.path();
    for (int i = 0; i < path.mapping_size(); ++i) {
        nodes.insert(path.mapping(i).position().node_id());
    }
    VG graph;
    for (auto& node : nodes) {
        *graph.graph.add_node() = xindex->node(node);
    }
    xindex->expand_context(graph.graph, max(1, context_size), false); // get connected edges
    graph.rebuild_indexes();
    return graph;
}

// estimate the fragment length as the difference in mean positions of both alignments
map<string, int> Mapper::approx_pair_fragment_length(const Alignment& aln1, const Alignment& aln2) {
    map<string, int> lengths;
    auto pos1 = alignment_mean_path_positions(aln1);
    auto pos2 = alignment_mean_path_positions(aln2);
    for (auto& p : pos1) {
        auto x = pos2.find(p.first);
        if (x != pos2.end()) {
            lengths[p.first] = x->second - p.second;
        }
    }
    return lengths;
}

void Mapper::record_fragment_configuration(int length, const Alignment& aln1, const Alignment& aln2) {
    // record the relative orientations
    assert(aln1.path().mapping(0).has_position() && aln2.path().mapping(0).has_position());
    bool aln1_is_rev = aln1.path().mapping(0).position().is_reverse();
    bool aln2_is_rev = aln2.path().mapping(0).position().is_reverse();
    bool same_orientation = aln1_is_rev == aln2_is_rev;
    fragment_orientations.push_front(same_orientation);
    if (fragment_orientations.size() > fragment_length_cache_size) {
        fragment_orientations.pop_back();
    }
    // assuming a dag-like graph
    // which direction do we go relative to the orientation of our first mate to find the second?
    bool same_direction = true;
    if (aln1_is_rev && length <= 0) {
        same_direction = true;
    } else if (!aln1_is_rev && length >= 0) {
        same_direction = true;
    } else if (aln1_is_rev && length >= 0) {
        same_direction = false;
    } else if (!aln1_is_rev && length <= 0) {
        same_direction = false;
    } else {
        assert(false);
    }
    fragment_directions.push_front(same_direction);
    if (fragment_directions.size() > fragment_length_cache_size) {
        fragment_directions.pop_back();
    }
    // assume we can record the fragment length
    fragment_lengths.push_front(abs(length));
    if (fragment_lengths.size() > fragment_length_cache_size) {
        auto last = fragment_lengths.back();
        fragment_lengths.pop_back();
    }
    if (++since_last_fragment_length_estimate > fragment_length_estimate_interval) {
        cached_fragment_length_mean = fragment_length_mean();
        cached_fragment_length_stdev = fragment_length_stdev();
        cached_fragment_orientation = fragment_orientation();
        cached_fragment_direction = fragment_direction();
        // set our fragment size cap to the cached mean + 10x the standard deviation
        fragment_size = cached_fragment_length_mean + fragment_sigma * cached_fragment_length_stdev;
        since_last_fragment_length_estimate = 1;
    }
}

double Mapper::fragment_length_stdev(void) {
    return stdev(fragment_lengths);
}

double Mapper::fragment_length_mean(void) {
    double sum = std::accumulate(fragment_lengths.begin(), fragment_lengths.end(), 0.0);
    return sum / fragment_lengths.size();
}

double Mapper::fragment_length_pdf(double length) {
    return normal_pdf(length, cached_fragment_length_mean, cached_fragment_length_stdev);
}

bool Mapper::fragment_orientation(void) {
    int count_same = 0;
    int count_diff = 0;
    for (auto& same_strand : fragment_orientations) {
        if (same_strand) ++count_same;
        else ++count_diff;
    }
    return count_same > count_diff;
}

bool Mapper::fragment_direction(void) {
    int count_fwd = 0;
    int count_rev = 0;
    for (auto& go_forward : fragment_directions) {
        if (go_forward) ++count_fwd;
        else ++count_rev;
    }
    return count_fwd > count_rev;
}

set<MaximalExactMatch*> Mapper::resolve_paired_mems(vector<MaximalExactMatch>& mems1,
                                                    vector<MaximalExactMatch>& mems2) {
    // find the MEMs that are within estimated_fragment_size of each other

    set<MaximalExactMatch*> pairable;

    // do a wide clustering and then do all pairs within each cluster
    // we will use these to determine the alignment strand
    //map<id_t, StrandCounts> node_strands;
    // records a mapping of id->MEMs, for cluster ranking
    map<id_t, vector<MaximalExactMatch*> > id_to_mems;
    // for clustering
    set<id_t> ids1, ids2;
    vector<id_t> ids;

    // run through the mems
    for (auto& mem : mems1) {
        for (auto& node : mem.nodes) {
            id_t id = gcsa::Node::id(node);
            id_to_mems[id].push_back(&mem);
            ids1.insert(id);
            ids.push_back(id);
        }
    }
    for (auto& mem : mems2) {
        for (auto& node : mem.nodes) {
            id_t id = gcsa::Node::id(node);
            id_to_mems[id].push_back(&mem);
            ids2.insert(id);
            ids.push_back(id);
        }
    }
    // remove duplicates
    //std::sort(ids.begin(), ids.end());
    //ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // get each hit's path-relative position
    map<string, map<int, vector<id_t> > > node_positions;
    for (auto& id : ids) {
        for (auto& ref : node_positions_in_paths(gcsa::Node::encode(id, 0))) {
            auto& name = ref.first;
            for (auto pos : ref.second) {
                node_positions[name][pos].push_back(id);
            }
        }
    }

    vector<vector<id_t> > clusters;
    for (auto& g : node_positions) {
        //if (g.second.empty()) continue; // should be impossible
        //cerr << g.first << endl;
        clusters.emplace_back();
        int prev = -1;
        for (auto& x : g.second) {
            auto cluster = &clusters.back();
            //auto& prev = clusters.back().back();
            auto curr = x.first;
            if(debug) {
                cerr << "p/c " << prev << " " << curr << endl;
            }
            if (prev != -1) {
                if (curr - prev <= fragment_size) {
                    // in cluster
#ifdef debug_mapper
#pragma omp critical
                    {
                        if (debug) {
                            cerr << "in cluster" << endl;
                        }
                    }
#endif
                } else {
                    // It's a new cluster
                    clusters.emplace_back();
                    cluster = &clusters.back();
                }
            }
            //cerr << " " << x.first << endl;
            for (auto& y : x.second) {
                //cerr << "  " << y << endl;
                cluster->push_back(y);
            }
            prev = curr;
        }
    }

    for (auto& cluster : clusters) {
        // for each pair of ids in the cluster
        // which are not from the same read
        // estimate the distance between them
        // we're roughly in the expected range
        bool has_first = false;
        bool has_second = false;
        for (auto& id : cluster) {
            has_first |= ids1.count(id);
            has_second |= ids2.count(id);
        }
        if (!has_first || !has_second) continue;
        for (auto& id : cluster) {
            for (auto& memptr : id_to_mems[id]) {
                pairable.insert(memptr);
            }
        }
    }

    return pairable;
}

// We need a function to get the lengths of nodes, in case we need to
// reverse an Alignment, including all its Mappings and Positions.
int64_t Mapper::get_node_length(int64_t node_id) {
    // Grab the node sequence only from the XG index and get its size.
    // Make sure to use the cache
    return xg_cached_node_length(node_id, xindex, get_node_cache());
}

bool Mapper::check_alignment(const Alignment& aln) {
    // use the graph to extract the sequence
    // assert that this == the alignment
    if (aln.path().mapping_size()) {
        // get the graph corresponding to the alignment path
        Graph sub;
        for (int i = 0; i < aln.path().mapping_size(); ++ i) {
            auto& m = aln.path().mapping(i);
            if (m.has_position() && m.position().node_id()) {
                auto id = aln.path().mapping(i).position().node_id();
                // XXXXXX this is single-threaded!
                xindex->neighborhood(id, 2, sub);
            }
        }
        VG g; g.extend(sub);
        auto seq = g.path_string(aln.path());
        //if (aln.sequence().find('N') == string::npos && seq != aln.sequence()) {
        if (aln.quality().size() && aln.quality().size() != aln.sequence().size()) {
            cerr << "alignment quality is not the same length as its sequence" << endl
                 << pb2json(aln) << endl;
            return false;
        }
        if (seq != aln.sequence()) {
            cerr << "alignment does not match graph " << endl
                 << pb2json(aln) << endl
                 << "expect:\t" << aln.sequence() << endl
                 << "got:\t" << seq << endl;
            // save alignment
            write_alignment_to_file(aln, "fail-" + hash_alignment(aln) + ".gam");
            // save graph, bigger fragment
            xindex->expand_context(sub, 5, true);
            VG gn; gn.extend(sub);
            gn.serialize_to_file("fail-" + gn.hash() + ".vg");
            return false;
        }
    }
    return true;
}

Alignment Mapper::align_banded(const Alignment& read, int kmer_size, int stride, int max_mem_length, int band_width) {
    // split the alignment up into overlapping chunks of band_width size
    list<Alignment> alignments;
    // force used bandwidth to be divisible by 4
    // round up so we have > band_width
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "trying band width " << band_width << endl;
        }
    }
#endif
    if (band_width % 4) {
        band_width -= band_width % 4; band_width += 4;
    }
    assert(read.sequence().size() > band_width);
    int div = 2;
    while (read.sequence().size()/div > band_width) {
        ++div;
    }
    int segment_size = read.sequence().size()/div;
    // use segment sizes divisible by 4, as this simplifies math
    // round up as well
    // we'll divide the overlap by 2 and 2 and again when stripping from the start
    // and end of sub-reads
    if (segment_size % 4) {
        segment_size -= segment_size % 4; segment_size += 4;
    }
#ifdef debug_mapper
    if (debug) {
        cerr << "Segment size be " << segment_size << "/" << read.sequence().size() << endl;
    }
#endif
    // and overlap them too
    size_t to_align = div * 2 - 1; // number of alignments we'll do
    vector<pair<size_t, size_t>> to_strip; to_strip.resize(to_align);
    vector<Alignment> bands; bands.resize(to_align);

    // scan across the read choosing bands
    // these bands are hard coded to overlap by 50%
    // the last band is guaranteed to be segment_size long
    // overlap scheme example
    // read: ----------------------
    //       --------
    //           --------
    //               --------
    //                   --------
    //                     --------
    // Afterwards, we align each fragment, trim the overlaps implied by the layout
    // and concatenate the alignments. The result is a split read alignment that
    // can describe large indels, CNVs, and inversions natively, even though our
    // local alignment algorithm is only aware of alignment against DAGs.
    for (int i = 0; i < div; ++i) {
        size_t off = i*segment_size;
        // TODO: copying the whole read here, including sequence and qualities,
        // makes this O(n^2).
        Alignment aln = read;
        size_t addl_seq = 0;
        if (i+1 == div) {
            // ensure we have a full-length segment for the last alignment
            // otherwise we run the risk of trying to align a very tiny band
            size_t last_off = read.sequence().size() - segment_size;
            if (off > last_off) {
                // looks wrawng
                addl_seq = (off - last_off);
                aln.set_sequence(read.sequence().substr(last_off));
                //assert(aln.sequence().size() == segment_size);
            } else {
                aln.set_sequence(read.sequence().substr(off));
            }
        } else {
            aln.set_sequence(read.sequence().substr(off, segment_size));
        }
        size_t idx = 2*i;
        to_strip[idx].first = (i == 0 ? 0 : segment_size/4 + addl_seq);
        to_strip[idx].second = (i+1 == div ? 0 : segment_size/4);
        bands[idx] = aln;
        if (i != div-1) { // if we're not at the last sequence
            aln.set_sequence(read.sequence().substr(off+segment_size/2,
                                                    segment_size));
            idx = 2*i+1;
            to_strip[idx].first = segment_size/4;
            // record second but take account of case where we run off end
            to_strip[idx].second = segment_size/4 - (segment_size - aln.sequence().size());
            bands[idx] = aln;
        }
    }

    vector<vector<Alignment>> multi_alns;
    vector<Alignment> alns;
    if (max_multimaps > 1) multi_alns.resize(to_align);
    else alns.resize(to_align);

    auto do_band = [&](int i) {
        if (max_multimaps > 1) {
            vector<Alignment>& malns = multi_alns[i];
            double cluster_mq = 0;
            malns = align_multi_internal(false, bands[i], kmer_size, stride, max_mem_length, band_width, cluster_mq, extra_multimaps, nullptr);
            // always include an unaligned mapping
            malns.push_back(bands[i]);
            for (vector<Alignment>::iterator a = malns.begin(); a != malns.end(); ++a) {
                Alignment& aln = *a;
                bool above_threshold = aln.identity() >= min_identity;
                if (!above_threshold) {
                    // treat as unmapped
                    aln = bands[i];
                }
                // strip overlaps
                aln = strip_from_start(aln, to_strip[i].first);
                aln = strip_from_end(aln, to_strip[i].second);
            }
        } else {
            Alignment& aln = alns[i];
            aln = align(bands[i], kmer_size, stride, max_mem_length, band_width);
            bool above_threshold = aln.identity() >= min_identity;
            if (!above_threshold) {
                aln = bands[i]; // unmapped
            }
            
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) {
                    cerr << "Unstripped alignment: " << pb2json(aln) << endl;
                }
            }
#endif
            
            // strip overlaps
            //cerr << "checking before strip" << endl;
            //check_alignment(aln);
            // clean up null positions that confuse stripping
            for (int j = 0; j < aln.path().mapping_size(); ++j) {
                auto* mapping = aln.mutable_path()->mutable_mapping(j);
                if (mapping->has_position() && !mapping->position().node_id()) {
                    mapping->clear_position();
                }
            }
            aln = strip_from_start(aln, to_strip[i].first);
            aln = strip_from_end(aln, to_strip[i].second);
            //cerr << "checking after strip" << endl;
            //check_alignment(aln);
            //cerr << "OK" << endl;
        }
    };
    
    if (alignment_threads > 1) {
#pragma omp parallel for schedule(dynamic,1)
        for (int i = 0; i < bands.size(); ++i) {
            do_band(i);
        }
    } else {
        for (int i = 0; i < bands.size(); ++i) {
            do_band(i);
        }
    }

    // resolve the highest-scoring traversal of the multi-mappings
    if (max_multimaps > 1) {
        alns = resolve_banded_multi(multi_alns);
        multi_alns.clear(); // clean up
    }

    // check that the alignments are valid
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            for (auto& aln : alns) {
                check_alignment(aln);
            }
        }
    }
#endif

    // merge the resulting alignments
    Alignment merged = merge_alignments(alns);

    merged.set_score(score_alignment(merged));
    merged.set_identity(identity(merged.path()));
    merged.set_quality(read.quality());
    merged.set_name(read.name());

    if(debug) {
        for(int i = 0; i < merged.path().mapping_size(); i++) {
            // Check each Mapping to make sure it doesn't go past the end of its
            // node.
            auto& mapping = merged.path().mapping(i);

            // What node is the mapping on
            int64_t node_id = mapping.position().node_id();
            if(node_id != 0) {
                // If it's actually on a node, get the node's sequence length
                int64_t node_length = get_node_length(node_id);

                // Make sure the mapping from length is shorter than the node length
                assert(node_length >= mapping_from_length(mapping));
            }
        }
    }

    return merged;
}

vector<Alignment> Mapper::resolve_banded_multi(vector<vector<Alignment>>& multi_alns) {
    // use a basic dynamic programming to score the path through the multi mapping
    // we add the score as long as our alignments overlap (we expect them to)
    // otherwise we add nothing
    // reads that are < the minimum alignment score threshold are dropped

    // a vector of
    // score, current alignment, parent alignment (direction)
    typedef tuple<int, Alignment*, size_t> score_t;
    vector<vector<score_t>> scores;
    scores.resize(multi_alns.size());
    // start with the scores for the first alignments
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) {
            cerr << "resolving banded multi over:" << endl;
            for (auto& alns : multi_alns) {
                for (auto& aln : alns) {
                    if (aln.has_path()) {
                        cerr << aln.score() << "@ " << make_pos_t(aln.path().mapping(0).position()) <<", ";
                    }
                }
                cerr << endl;
            }
        }
    }
#endif
    for (auto& aln : multi_alns[0]) {
        scores.front().push_back(make_tuple(aln.score(), &aln, 0));
    }
    for (size_t i = 1; i < multi_alns.size(); ++i) {
        auto& curr_alns = multi_alns[i];
        vector<score_t>& curr_scores = scores[i];
        auto& prev_scores = scores[i-1];
        // find the best previous score
        score_t best_prev = prev_scores.front();
        size_t best_idx = 0;
        score_t unmapped_prev = prev_scores.front();
        size_t unmapped_idx = 0;
        size_t j = 0;
        for (auto& t : prev_scores) {
            if (get<0>(t) > get<0>(best_prev)) {
                best_prev = t;
                best_idx = j;
            }
            if (get<0>(t) == 0) {
                unmapped_idx = j;
                unmapped_prev = t;
            }
            ++j;
        }
        // for each alignment
        for (auto& aln : curr_alns) {
            // if it's not mapped, take the best previous score
            if (!aln.score()) {
                curr_scores.push_back(make_tuple(get<0>(best_prev),
                                                 &aln, best_idx));
            } else {
                // determine our start
                auto& curr_start = aln.path().mapping(0).position();
                // accumulate candidate alignments
                map<int, vector<pair<score_t, size_t>>> candidates;
                // for each previous alignment
                size_t k = 0;
                for (auto& score : prev_scores) {
                    auto old = get<1>(score);
                    if (!old->score()) continue; // unmapped
                    auto prev_end = path_end(old->path());
                    // save it as a candidate if the two are adjacent
                    // and in the same orientation
                    if (adjacent_positions(prev_end, curr_start)) {
                        candidates[get<0>(score)].push_back(make_pair(score,k));
                    }
                    ++k;
                }
                if (candidates.size()) {
                    // take the best one (at least the first best one we saw)
                    auto& opt = candidates.rbegin()->second.front();
                    // DP scoring step: add scores when we match head to tail
                    curr_scores.push_back(make_tuple(get<0>(opt.first) + aln.score(),
                                                     &aln, opt.second));
                } else {
                    // if there are no alignments matching our start
                    // just take the highest-scoring one
                    auto best_prev_aln = get<1>(prev_scores[best_idx]);
                    if (best_prev_aln->has_path()) {
                        curr_scores.push_back(make_tuple(get<0>(best_prev),
                                                         &aln, best_idx));
                    } else {
                        curr_scores.push_back(make_tuple(get<0>(unmapped_prev),
                                                         &aln, unmapped_idx));
                    }
                }
            }
        }
    }
    // find the best score at the end
    score_t best_last = scores.back().front();
    size_t best_last_idx = 0;
    size_t j = 0;
    for (auto& s : scores.back()) {
        if (get<0>(s) > get<0>(best_last)) {
            best_last = s;
            best_last_idx = j;
        }
        ++j;
    }
    // accumulate the alignments in the optimal path
    vector<Alignment> alns; alns.resize(multi_alns.size());
    size_t prev_best_idx = best_last_idx;
    for (int i = scores.size()-1; i >= 0; --i) {
        auto& score = scores[i][prev_best_idx];
        alns[i] = *get<1>(score); // save the alignment
        prev_best_idx = get<2>(score); // and where we go next
    }
    return alns;
}

bool Mapper::adjacent_positions(const Position& pos1, const Position& pos2) {
    // are they the same id, with offset differing by 1?
    if (pos1.node_id() == pos2.node_id()
        && pos1.offset() == pos2.offset()-1) {
        return true;
    }
    // otherwise, we're going to need to check via the index
    VG graph;
    // pick up a graph that's just the neighborhood of the start and end positions
    int64_t id1 = pos1.node_id();
    int64_t id2 = pos2.node_id();
    if(xindex) {
        // Grab the node sequence only from the XG index and get its size.
        xindex->get_id_range(id1, id1, graph.graph);
        xindex->get_id_range(id2, id2, graph.graph);
        xindex->expand_context(graph.graph, 1, false);
        graph.rebuild_indexes();
    } else if(index) {
        index->get_context(id1, graph);
        index->get_context(id2, graph);
        index->expand_context(graph, 1);
    } else {
        throw runtime_error("No index to get nodes from.");
    }
    // now look in the graph to figure out if we are adjacent
    return graph.adjacent(pos1, pos2);
}

QualAdjAligner* Mapper::get_qual_adj_aligner(void) {
    int tid = qual_adj_aligners.size() > 1 ? omp_get_thread_num() : 0;
    return qual_adj_aligners[tid];
}

Aligner* Mapper::get_regular_aligner(void) {
    int tid = regular_aligners.size() > 1 ? omp_get_thread_num() : 0;
    return regular_aligners[tid];
}

LRUCache<id_t, Node>& Mapper::get_node_cache(void) {
    int tid = node_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *node_cache[tid];
}

LRUCache<id_t, size_t>& Mapper::get_node_start_cache(void) {
    int tid = node_start_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *node_start_cache[tid];
}

LRUCache<gcsa::node_type, map<string, vector<size_t> > >& Mapper::get_node_pos_cache(void) {
    int tid = node_pos_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *node_pos_cache[tid];
}

LRUCache<id_t, vector<Edge> >& Mapper::get_edge_cache(void) {
    int tid = edge_cache.size() > 1 ? omp_get_thread_num() : 0;
    return *edge_cache[tid];
}

void Mapper::compute_mapping_qualities(vector<Alignment>& alns, double cluster_mq, double mq_estimate, double mq_cap) {
    if (alns.empty()) return;
    double max_mq = min(mq_cap, (double)max_mapping_quality);
    auto aligner = (alns.front().quality().empty() ? (BaseAligner*) get_regular_aligner() : (BaseAligner*) get_qual_adj_aligner());
    int sub_overlaps = sub_overlaps_of_first_aln(alns, mq_overlap);
    switch (mapping_quality_method) {
        case Approx:
            aligner->compute_mapping_quality(alns, max_mq, true, cluster_mq, use_cluster_mq, sub_overlaps, mq_estimate);
            break;
        case Exact:
            aligner->compute_mapping_quality(alns, max_mq, false, cluster_mq, use_cluster_mq, sub_overlaps, mq_estimate);
            break;
        default: // None
            break;
    }
}
    
void Mapper::compute_mapping_qualities(pair<vector<Alignment>, vector<Alignment>>& pair_alns, double cluster_mq, double mq_estimate1, double mq_estimate2, double mq_cap1, double mq_cap2) {
    if (pair_alns.first.empty() || pair_alns.second.empty()) return;
    double max_mq1 = min(mq_cap1, (double)max_mapping_quality);
    double max_mq2 = min(mq_cap2, (double)max_mapping_quality);
    auto aligner = (pair_alns.first.front().quality().empty() ? (BaseAligner*) get_regular_aligner() : (BaseAligner*) get_qual_adj_aligner());
    int sub_overlaps1 = sub_overlaps_of_first_aln(pair_alns.first, mq_overlap);
    int sub_overlaps2 = sub_overlaps_of_first_aln(pair_alns.second, mq_overlap);
    switch (mapping_quality_method) {
        case Approx:
            aligner->compute_paired_mapping_quality(pair_alns, max_mq1, max_mq2, true, cluster_mq, use_cluster_mq, sub_overlaps1, sub_overlaps2, mq_estimate1, mq_estimate2);
            break;
        case Exact:
            aligner->compute_paired_mapping_quality(pair_alns, max_mq1, max_mq2, false, cluster_mq, use_cluster_mq, sub_overlaps1, sub_overlaps2, mq_estimate1, mq_estimate2);
            break;
        default: // None
            break;
    }
}

double Mapper::estimate_max_possible_mapping_quality(int length, double min_diffs, double next_min_diffs) {
    auto aligner = (BaseAligner*) get_regular_aligner();
    return aligner->estimate_max_possible_mapping_quality(length, min_diffs, next_min_diffs);
}

vector<Alignment> Mapper::score_sort_and_deduplicate_alignments(vector<Alignment>& all_alns, const Alignment& original_alignment) {
    if (all_alns.size() == 0) {
        all_alns.emplace_back();
        Alignment& aln = all_alns.back();
        aln = original_alignment;
        aln.clear_path();
        aln.set_score(0);
        return all_alns;
    }
    
    map<int, set<Alignment*> > alignment_by_score;
    for (auto& ta : all_alns) {
        Alignment* aln = &ta;
        alignment_by_score[aln->score()].insert(aln);
    }
    // TODO: Filter down subject to a minimum score per base or something?
    // Collect all the unique alignments (to compute mapping quality) and order by score
    vector<Alignment> sorted_unique_alignments;
    for(auto it = alignment_by_score.rbegin(); it != alignment_by_score.rend(); ++it) {
        // Copy over all the alignments in descending score order (following the pointers into the "alignments" vector)
        // Iterating through a set keyed on ints backward is in descending order.
        
        // This is going to let us deduplicate our alignments with this score, by storing them serialized to strings in this set.
        set<string> serializedAlignmentsUsed;
        
        for(Alignment* pointer : (*it).second) {
            // We serialize the alignment to a string
            string serialized;
            pointer->SerializeToString(&serialized);
            
            if(!serializedAlignmentsUsed.count(serialized)) {
                // This alignment hasn't been produced yet. Produce it. The
                // order in the alignment vector doesn't matter for things with
                // the same score.
                sorted_unique_alignments.push_back(*pointer);
                
                // Save it so we can avoid putting it in the vector again
                serializedAlignmentsUsed.insert(serialized);
            }
        }
    }
    return sorted_unique_alignments;
}

// filters down to requested number of alignments and marks
void Mapper::filter_and_process_multimaps(vector<Alignment>& sorted_unique_alignments, int additional_multimaps) {
    int total_multimaps = max_multimaps + additional_multimaps;
    if (sorted_unique_alignments.size() > total_multimaps){
        sorted_unique_alignments.resize(total_multimaps);
    }
    
    // TODO log best alignment score?
    for(size_t i = 0; i < sorted_unique_alignments.size(); i++) {
        // Mark all but the first, best alignment as secondary
        sorted_unique_alignments[i].set_is_secondary(i > 0);
    }
}
    
vector<Alignment> Mapper::align_multi(const Alignment& aln, int kmer_size, int stride, int max_mem_length, int band_width) {
    double cluster_mq = 0;
    return align_multi_internal(true, aln, kmer_size, stride, max_mem_length, band_width, cluster_mq, extra_multimaps, nullptr);
}
    
vector<Alignment> Mapper::align_multi_internal(bool compute_unpaired_quality,
                                               const Alignment& aln,
                                               int kmer_size, int stride,
                                               int max_mem_length,
                                               int band_width,
                                               double& cluster_mq,
                                               int additional_multimaps,
                                               vector<MaximalExactMatch>* restricted_mems) {
    
    if(debug) {
#pragma omp critical
        cerr << "align_multi_internal("
            << compute_unpaired_quality << ", " 
            << aln.sequence() << ", " 
            << kmer_size << ", " 
            << stride << ", " 
            << band_width << ", " 
            << additional_multimaps << ", " 
            << restricted_mems << ")" 
            << endl;
        if (aln.has_path()) {
            // if we're realigning, show in the debugging output what we start with
            cerr << pb2json(aln) << endl;
        }
    }
    
    // trigger a banded alignment if we need to
    // note that this will in turn call align_multi_internal on fragments of the read
    if (aln.sequence().size() > band_width) {
        // TODO: banded alignment currently doesn't support mapping qualities because it only produces one alignment
#ifdef debug_mapper
#pragma omp critical
        if (debug) cerr << "switching to banded alignment" << endl;
#endif
        return vector<Alignment>{align_banded(aln, kmer_size, stride, max_mem_length, band_width)};
    }
    
    // try to get at least 2 multimaps so that we can calculate mapping quality
    int additional_multimaps_for_quality;
    if (additional_multimaps == 0 && max_multimaps == 1 && mapping_quality_method != None) {
        additional_multimaps_for_quality = 1;
    }
    else {
        additional_multimaps_for_quality = additional_multimaps;
    }

    double longest_lcp;
    vector<Alignment> alignments;
    
    // use pre-restricted mems for paired mapping or find mems here
    if (restricted_mems != nullptr) {
        // mem hits will already have been queried
        alignments = align_mem_multi(aln, *restricted_mems, cluster_mq, longest_lcp, max_mem_length, additional_multimaps_for_quality);
    }
    else {
        vector<MaximalExactMatch> mems = find_mems_deep(aln.sequence().begin(),
                                                        aln.sequence().end(),
                                                        longest_lcp,
                                                        max_mem_length,
                                                        min_mem_length,
                                                        mem_reseed_length);
        // query mem hits
        alignments = align_mem_multi(aln, mems, cluster_mq, longest_lcp, max_mem_length, additional_multimaps_for_quality);
    }

    /*
    alignments = score_sort_and_deduplicate_alignments(alignments, aln);
    
    // compute mapping quality before removing extra alignments
    if (compute_unpaired_quality) {
        compute_mapping_qualities(alignments, cluster_mq, longest_lcp, max_mapping_quality);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) cerr << "mapping quality " << alignments.front().mapping_quality() << " " << cluster_mq << endl;
        }
#endif
        filter_and_process_multimaps(alignments, 0);
    } else {
        filter_and_process_multimaps(alignments, additional_multimaps);
    }
    */

    for (auto& aln : alignments) {
        // Make sure no alignments are wandering out of the graph
        for (size_t i = 0; i < aln.path().mapping_size(); i++) {
            // Look at each mapping
            auto& mapping = aln.path().mapping(i);
            
            if (mapping.position().node_id()) {
                // Get the size of its node from whatever index we have
                size_t node_size = get_node_length(mapping.position().node_id());
                
                // Make sure the mapping fits in the node
                assert(mapping.position().offset() + mapping_from_length(mapping) <= node_size);
            }
        }
    }
    
    return alignments;
}

Alignment Mapper::align(const Alignment& aln, int kmer_size, int stride, int max_mem_length, int band_width) {
    // TODO computing mapping quality could be inefficient depending on the method chosen
    
    // Do the multi-mapping
    vector<Alignment> best = align_multi(aln, kmer_size, stride, max_mem_length, band_width);

    if(best.size() == 0) {
        // Spit back an alignment that says we failed, but make sure it has the right sequence in it.
        Alignment failed = aln;
        failed.clear_path();
        failed.set_score(0);
        return failed;
    }

    // Otherwise, just report the best alignment, since we know one exists
    return best[0];
}

set<pos_t> gcsa_nodes_to_positions(const vector<gcsa::node_type>& nodes) {
    set<pos_t> positions;
    for(gcsa::node_type node : nodes) {
        positions.insert(make_pos_t(node));
    }
    return positions;    
}

set<pos_t> Mapper::sequence_positions(const string& seq) {
    gcsa::range_type gcsa_range = gcsa->find(seq);
    std::vector<gcsa::node_type> gcsa_nodes;
    gcsa->locate(gcsa_range, gcsa_nodes);
    return gcsa_nodes_to_positions(gcsa_nodes);
}

// Use the GCSA2 index to find super-maximal exact matches.
vector<MaximalExactMatch>
Mapper::find_mems_simple(string::const_iterator seq_begin,
                         string::const_iterator seq_end,
                         int max_mem_length,
                         int min_mem_length,
                         int reseed_length) {

    if (!gcsa) {
        cerr << "error:[vg::Mapper] a GCSA2 index is required to query MEMs" << endl;
        exit(1);
    }

    string::const_iterator cursor = seq_end;
    vector<MaximalExactMatch> mems;

    // an empty sequence matches the entire bwt
    if (seq_begin == seq_end) {
        mems.emplace_back(
            MaximalExactMatch(seq_begin, seq_end,
                              gcsa::range_type(0, gcsa->size() - 1)));
        return mems;
    }
    
    // find SMEMs using GCSA+LCP array
    // algorithm sketch:
    // set up a cursor pointing to the last position in the sequence
    // set up a structure to track our MEMs, and set it == "" and full range match
    // while our cursor is >= the beginning of the string
    //   try a step of backwards searching using LF mapping
    //   if our range goes to 0
    //       go back to the last non-empty range
    //       emit the MEM corresponding to this range
    //       start a new mem
    //           use the LCP array's parent function to cut off the end of the match
    //           (effectively, this steps up the suffix tree)
    //           and calculate the new end point using the LCP of the parent node
    // emit the final MEM, if we finished in a matching state

    // the temporary MEM we'll build up in this process
    auto full_range = gcsa::range_type(0, gcsa->size() - 1);
    MaximalExactMatch match(cursor, cursor, full_range);
    gcsa::range_type last_range = match.range;
    --cursor; // start off looking at the last character in the query
    while (cursor >= seq_begin) {
        // hold onto our previous range
        last_range = match.range;
        // execute one step of LF mapping
        match.range = gcsa->LF(match.range, gcsa->alpha.char2comp[*cursor]);
        if (gcsa::Range::empty(match.range)
            || max_mem_length && match.end-cursor > max_mem_length
            || match.end-cursor > gcsa->order()) {
            // break on N; which for DNA we assume is non-informative
            // this *will* match many places in assemblies; this isn't helpful
            if (*cursor == 'N' || last_range == full_range) {
                // we mismatched in a single character
                // there is no MEM here
                match.begin = cursor+1;
                match.range = last_range;
                mems.push_back(match);
                match.end = cursor;
                match.range = full_range;
                --cursor;
            } else {
                // we've exhausted our BWT range, so the last match range was maximal
                // or: we have exceeded the order of the graph (FPs if we go further)
                //     we have run over our parameter-defined MEM limit
                // record the last MEM
                match.begin = cursor+1;
                match.range = last_range;
                mems.push_back(match);
                // set up the next MEM using the parent node range
                // length of last MEM, which we use to update our end pointer for the next MEM
                size_t last_mem_length = match.end - match.begin;
                // get the parent suffix tree node corresponding to the parent of the last MEM's STNode
                gcsa::STNode parent = lcp->parent(last_range);
                // change the end for the next mem to reflect our step size
                size_t step_size = last_mem_length - parent.lcp();
                match.end = mems.back().end-step_size;
                // and set up the next MEM using the parent node range
                match.range = parent.range();
            }
        } else {
            // we are matching
            match.begin = cursor;
            // just step to the next position
            --cursor;
        }
    }
    // if we have a non-empty MEM at the end, record it
    if (match.end - match.begin > 0) mems.push_back(match);

    // find the SMEMs from the mostly-SMEM and some MEM list we've built
    // FIXME: un-hack this (it shouldn't be needed!)
    // the algorithm sometimes generates MEMs contained in SMEMs
    // with the pattern that they have the same beginning position
    map<string::const_iterator, string::const_iterator> smems_begin;
    for (auto& mem : mems) {
        auto x = smems_begin.find(mem.begin);
        if (x == smems_begin.end()) {
            smems_begin[mem.begin] = mem.end;
        } else {
            if (x->second < mem.end) {
                x->second = mem.end;
            }
        }
    }
    // remove zero-length entries and MEMs that aren't SMEMs
    // the zero-length ones are associated with single-base MEMs that tend to
    // match the entire index (typically Ns)
    // minor TODO: fix the above algorithm so they aren't introduced at all
    mems.erase(std::remove_if(mems.begin(), mems.end(),
                              [&smems_begin,
                               &min_mem_length](const MaximalExactMatch& m) {
                                  return ( m.end-m.begin == 0
                                           || m.length() < min_mem_length
                                           || smems_begin[m.begin] != m.end
                                           || m.count_Ns() > 0
                                      );
                              }),
               mems.end());
    // return the matches in natural order
    std::reverse(mems.begin(), mems.end());

    // fill the counts before deciding what to do
    for (auto& mem : mems) {
        if (mem.length() >= min_mem_length) {
            mem.match_count = gcsa->count(mem.range);
            if (mem.match_count > 0 && (!hit_max || mem.match_count <= hit_max)) {
                gcsa->locate(mem.range, mem.nodes);
            }
        }
    }
    
    // reseed the long smems with shorter mems
    if (reseed_length) {
        // find if there are any mems that should be reseeded
        // iterate through MEMs
        vector<MaximalExactMatch> reseeded;
        for (auto& mem : mems) {
            // reseed if we have a long singular match
            if (mem.length() >= reseed_length
                && mem.match_count == 1
                // or if we only have one mem for the entire read (even if it may have many matches)
                || mems.size() == 1) {
                // reseed at midway between here and the min mem length and at the min mem length
                int reseed_to = mem.length() / 2;
                int reseeds = 0;
                while (reseeds == 0 && reseed_to >= min_mem_length) {
#ifdef debug_mapper
#pragma omp critical
                    if (debug) cerr << "reseeding " << mem.sequence() << " with " << reseed_to << endl;
#endif
                    vector<MaximalExactMatch> remems = find_mems_simple(mem.begin,
                                                                        mem.end,
                                                                        reseed_to,
                                                                        min_mem_length,
                                                                        0);
                    reseed_to /= 2;
                    for (auto& rmem : remems) {
                        // keep if we have more than the match count of the parent
                        if (rmem.length() >= min_mem_length
                            && rmem.match_count > mem.match_count) {
                            ++reseeds;
                            reseeded.push_back(rmem);
                        }
                    }
                }
                // at least keep the original mem if needed
                if (reseeds == 0) {
                    reseeded.push_back(mem);
                }
            } else {
                reseeded.push_back(mem);
            }
        }
        mems = reseeded;
        // re-sort the MEMs by their start position
        std::sort(mems.begin(), mems.end(), [](const MaximalExactMatch& m1, const MaximalExactMatch& m2) { return m1.begin < m2.begin; });
    }
    // print the matches
    /*
    for (auto& mem : mems) {
        cerr << mem << endl;
    }
    */
    // verify the matches (super costly at scale)
    /*
#ifdef debug_mapper
    if (debug) { check_mems(mems); }
#endif
    */
    return mems;
}

// Use the GCSA2 index to find super-maximal exact matches (and optionally sub-MEMs).
vector<MaximalExactMatch> Mapper::find_mems_deep(string::const_iterator seq_begin,
                                                 string::const_iterator seq_end,
                                                 double& longest_lcp,
                                                 int max_mem_length,
                                                 int min_mem_length,
                                                 int reseed_length) {
    
#ifdef debug_mapper
#pragma omp critical
    {
        cerr << "find_mems: sequence ";
        for (auto iter = seq_begin; iter != seq_end; iter++) {
            cerr << *iter;
        }
        cerr << ", max mem length " << max_mem_length << ", min mem length " <<
                min_mem_length << ", reseed length " << reseed_length << endl;
    }
#endif
    
    
    if (!gcsa) {
        cerr << "error:[vg::Mapper] a GCSA2 index is required to query MEMs" << endl;
        exit(1);
    }
    
    if (min_mem_length > reseed_length && reseed_length) {
        cerr << "error:[vg::Mapper] minimimum reseed length for MEMs cannot be less than minimum MEM length" << endl;
        exit(1);
    }
    vector<MaximalExactMatch> mems;
    vector<pair<MaximalExactMatch, vector<size_t> > > sub_mems;

    gcsa::range_type full_range = gcsa::range_type(0, gcsa->size() - 1);
    
    // an empty sequence matches the entire bwt
    if (seq_begin == seq_end) {
        mems.push_back(MaximalExactMatch(seq_begin, seq_end, full_range));
    }
    
    // find SMEMs using GCSA+LCP array
    // algorithm sketch:
    // set up a cursor pointing to the last position in the sequence
    // set up a structure to track our MEMs, and set it == "" and full range match
    // while our cursor is >= the beginning of the string
    //   try a step of backwards searching using LF mapping
    //   if our range goes to 0
    //       go back to the last non-empty range
    //       emit the MEM corresponding to this range
    //       start a new mem
    //           use the LCP array's parent function to cut off the end of the match
    //           (effectively, this steps up the suffix tree)
    //           and calculate the new end point using the LCP of the parent node
    // emit the final MEM, if we finished in a matching state
    
    // next position we will extend matches to
    string::const_iterator cursor = seq_end - 1;
    
    // range of the last iteration
    gcsa::range_type last_range = full_range;
    
    // the temporary MEM we'll build up in this process
    MaximalExactMatch match(cursor, seq_end, full_range);
    
    // did we move the cursor or the end of the match last iteration?
    bool prev_iter_jumped_lcp = false;

    int max_lcp = 0;
    vector<int> lcp_maxima;
    
    // loop maintains invariant that match.range contains the hits for seq[cursor+1:match.end]
    while (cursor >= seq_begin) {

        // break the MEM on N; which for DNA we assume is non-informative
        // this *will* match many places in assemblies, but it isn't helpful
        if (*cursor == 'N') {
            match.begin = cursor + 1;
            
            size_t mem_length = match.length();
            
            if (mem_length >= min_mem_length) {
                mems.push_back(match);
                
#ifdef debug_mapper
#pragma omp critical
                {
                    vector<gcsa::node_type> locations;
                    gcsa->locate(match.range, locations);
                    cerr << "adding MEM " << match.sequence() << " at positions ";
                    for (auto nt : locations) {
                        cerr << make_pos_t(nt) << " ";
                    }
                    cerr << endl;
                }
#endif
            }
            
            match.end = cursor;
            match.range = full_range;
            --cursor;
            
            // are we reseeding?
            if (reseed_length
                && mem_length >= min_mem_length
                && max_lcp >= reseed_length) {
                if (fast_reseed) {
                    find_sub_mems_fast(mems,
                                       match.end,
                                       max(min_mem_length, (int) mem_length / 2),
                                       sub_mems);
                }
                else {
                    find_sub_mems(mems,
                                  match.end,
                                  min_mem_length,
                                  sub_mems);
                }
            }

            prev_iter_jumped_lcp = false;
            lcp_maxima.push_back(max_lcp);
            max_lcp = 0;
            // skip looking for matches since they are non-informative
            continue;
        }
        
        // hold onto our previous range
        last_range = match.range;
        
        // execute one step of LF mapping
        match.range = gcsa->LF(match.range, gcsa->alpha.char2comp[*cursor]);
        
        if (gcsa::Range::empty(match.range)
            || (max_mem_length && match.end - cursor > max_mem_length)
            || match.end - cursor > gcsa->order()) {
            
            // we've exhausted our BWT range, so the last match range was maximal
            // or: we have exceeded the order of the graph (FPs if we go further)
            // or: we have run over our parameter-defined MEM limit
            
            if (cursor + 1 == match.end) {
                // avoid getting caught in infinite loop when a single character mismatches
                // entire index (b/c then advancing the LCP doesn't move the search forward
                // at all, need to move the cursor instead)
                match.begin = cursor + 1;
                match.range = last_range;
                
                if (match.end - match.begin >= min_mem_length) {
                    mems.push_back(match);
                }
                
                match.end = cursor;
                match.range = full_range;
                --cursor;
                
                // don't reseed in empty MEMs
                prev_iter_jumped_lcp = false;
                lcp_maxima.push_back(max_lcp);
                max_lcp = 0;
            }
            else {
                match.begin = cursor + 1;
                match.range = last_range;
                size_t mem_length = match.end - match.begin;
                
                // record the last MEM, but check to make sure were not actually still searching
                // for the end of the next MEM
                if (mem_length >= min_mem_length && !prev_iter_jumped_lcp) {
                    mems.push_back(match);

#ifdef debug_mapper
#pragma omp critical
                    {
                        vector<gcsa::node_type> locations;
                        gcsa->locate(match.range, locations);
                        cerr << "adding MEM " << match.sequence() << " at positions ";
                        for (auto nt : locations) {
                            cerr << make_pos_t(nt) << " ";
                        }
                        cerr << endl;
                    }
#endif
                }
                
                // get the parent suffix tree node corresponding to the parent of the last MEM's STNode
                gcsa::STNode parent = lcp->parent(last_range);
                // set the MEM to be the longest prefix that is shared with another MEM
                match.end = match.begin + parent.lcp();
                // and set up the next MEM using the parent node range
                match.range = parent.range();
                // record our max lcp
                //max_lcp = max(max_lcp, (int)parent.lcp());
                max_lcp = (int)parent.lcp();
                
                // are we reseeding?
                if (reseed_length
                    && !prev_iter_jumped_lcp
                    && mem_length >= min_mem_length
                    && max_lcp >= reseed_length) {
                    if (fast_reseed) {
                        find_sub_mems_fast(mems,
                                           match.end,
                                           max(min_mem_length, (int) mem_length / 2),
                                           sub_mems);
                    }
                    else {
                        find_sub_mems(mems,
                                      match.end,
                                      min_mem_length,
                                      sub_mems);
                    }
                }
                
                
                prev_iter_jumped_lcp = true;
                lcp_maxima.push_back(max_lcp);
                max_lcp = 0;
            }
        }
        else {
            prev_iter_jumped_lcp = false;
            //max_lcp = max((int)lcp->parent(match.range).lcp(), max_lcp);
            max_lcp = (int)lcp->parent(match.range).lcp();
            lcp_maxima.push_back(max_lcp);
            // just step to the next position
            --cursor;
        }
    }
    // TODO: is this where the bug with the duplicated MEMs is occurring? (when the prefix of a read
    // contains multiple non SMEM hits so that the iteration will loop through the LCP routine multiple
    // times before escaping out of the loop?
    
    // if we have a MEM at the beginning of the read, record it
    match.begin = seq_begin;
    size_t mem_length = match.end - match.begin;
    if (mem_length >= min_mem_length) {
        //max_lcp = max((int)lcp->parent(match.range).lcp(), max_lcp);
        max_lcp = (int)lcp->parent(match.range).lcp();
        mems.push_back(match);
        
#ifdef debug_mapper
#pragma omp critical
        {
            vector<gcsa::node_type> locations;
            gcsa->locate(match.range, locations);
            cerr << "adding MEM " << match.sequence() << " at positions ";
            for (auto nt : locations) {
                cerr << make_pos_t(nt) << " ";
            }
            cerr << endl;
        }
#endif
        
        // are we reseeding?
        if (reseed_length
            && mem_length >= min_mem_length
            && max_lcp >= reseed_length) {
            if (fast_reseed) {
                find_sub_mems_fast(mems,
                                   match.begin,
                                   max(min_mem_length, (int) mem_length / 2),
                                   sub_mems);
            }
            else {
                find_sub_mems(mems,
                              match.begin,
                              min_mem_length,
                              sub_mems);
            }
        }
    }
    lcp_maxima.push_back(max_lcp);
    longest_lcp = *max_element(lcp_maxima.begin(), lcp_maxima.end());

    // fill the MEMs' node lists and indicate they are primary MEMs
    for (MaximalExactMatch& mem : mems) {
        mem.match_count = gcsa->count(mem.range);
        mem.primary = true;
        // if we aren't filtering on hit count, or if we have up to the max allowed hits
        if (mem.match_count > 0 && (!hit_max || mem.match_count <= hit_max)) {
            // extract the graph positions matching the range
            gcsa->locate(mem.range, mem.nodes);
        }
    }
    
    if (reseed_length) {
        // determine counts of matches
        for (pair<MaximalExactMatch, vector<size_t> >& sub_mem_and_parents : sub_mems) {
            // count in entire range, including parents
            sub_mem_and_parents.first.match_count = gcsa->count(sub_mem_and_parents.first.range);
            // remove parents from count
            for (size_t parent_idx : sub_mem_and_parents.second) {
                sub_mem_and_parents.first.match_count -= mems[parent_idx].match_count;
            }
        }

        // fill MEMs with positions and set flag indicating they are submems
        for (auto& m : sub_mems) {
            auto& mem = m.first;
            mem.primary = false;
            if (mem.match_count > 0 && (!hit_max || mem.match_count <= hit_max)) {
                gcsa->locate(mem.range, mem.nodes);
            }
        }

        // combine the MEM and sub-MEM lists
        for (auto iter = sub_mems.begin(); iter != sub_mems.end(); iter++) {
            mems.push_back(std::move((*iter).first));
        }
        
    }
    
    // return the MEMs in order along the read
    // TODO: there should actually be a linear time method to merge and order the sub-MEMs, since
    // they are ordered by the parent MEMs
    std::sort(mems.begin(), mems.end(), [](const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
        return m1.begin < m2.begin ? true : (m1.begin == m2.begin ? m1.end < m2.end : false);
    });
    
    return mems;
}
    
void Mapper::find_sub_mems(vector<MaximalExactMatch>& mems,
                           string::const_iterator next_mem_end,
                           int min_mem_length,
                           vector<pair<MaximalExactMatch, vector<size_t>>>& sub_mems_out) {
    
    // get the most recently added MEM
    MaximalExactMatch& mem = mems.back();
    
#ifdef debug_mapper
#pragma omp critical
    {
        cerr << "find_sub_mems: sequence ";
        for (auto iter = mem.begin; iter != mem.end; iter++) {
            cerr << *iter;
        }
        cerr << ", min mem length " << min_mem_length << endl;
    }
#endif
    
    // how many times does the parent MEM occur in the index?
    size_t parent_count = gcsa->count(mem.range);
    
    // next position where we will look for a match
    string::const_iterator cursor = mem.end - 1;
    
    // the righthand end of the sub-MEM we are building
    string::const_iterator sub_mem_end = mem.end;
    
    // the range that matches search_start:sub_mem_end
    gcsa::range_type range = gcsa::range_type(0, gcsa->size() - 1);
    
    // did we move the cursor or the end of the match last iteration?
    bool prev_iter_jumped_lcp = false;
    
    // look for matches that are contained in this MEM and not contained in the next MEM
    while (cursor >= mem.begin && sub_mem_end > next_mem_end) {
        // Note: there should be no need to handle N's or whole-index mismatches in this
        // routine (unlike the SMEM routine) since they should never make it into a parent
        // SMEM in the first place
        
        // hold onto our previous range
        gcsa::range_type last_range = range;
        // execute one step of LF mapping
        range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);

        if (gcsa->count(range) <= parent_count) {
            // there are no more hits outside of parent MEM hits, record the previous
            // interval as a sub MEM
            string::const_iterator sub_mem_begin = cursor + 1;

            if (sub_mem_end - sub_mem_begin >= min_mem_length && !prev_iter_jumped_lcp) {
                sub_mems_out.push_back(make_pair(MaximalExactMatch(sub_mem_begin, sub_mem_end, last_range),
                                                 vector<size_t>{mems.size() - 1}));
#ifdef debug_mapper
#pragma omp critical
                {
                    vector<gcsa::node_type> locations;
                    gcsa->locate(last_range, locations);
                    cerr << "adding sub-MEM ";
                    for (auto iter = sub_mem_begin; iter != sub_mem_end; iter++) {
                        cerr << *iter;
                    }
                    cerr << " at positions ";
                    for (auto nt : locations) {
                        cerr << make_pos_t(nt) << " ";
                    }
                    cerr << endl;
                    
                }
#endif
                // identify all previous MEMs that also contain this sub-MEM
                for (int64_t i = ((int64_t) mems.size()) - 2; i >= 0; i--) {
                    if (sub_mem_begin >= mems[i].begin) {
                        // contined in next MEM, add its index to sub MEM's list of parents
                        sub_mems_out.back().second.push_back(i);
                    }
                    else {
                        // not contained in the next MEM, cannot be contained in earlier ones
                        break;
                    }
                }
            }
#ifdef debug_mapper
            else {
#pragma omp critical
                {
                    cerr << "minimally more frequent MEM is too short ";
                    for (auto iter = sub_mem_begin; iter != sub_mem_end; iter++) {
                        cerr << *iter;
                    }
                    cerr << endl;
                }
            }
#endif
            
            // get the parent suffix tree node corresponding to the parent of the last MEM's STNode
            gcsa::STNode parent = lcp->parent(last_range);
            // set the MEM to be the longest prefix that is shared with another MEM
            sub_mem_end = sub_mem_begin + parent.lcp();
            // and get the next range as parent node range
            range = parent.range();
            
            prev_iter_jumped_lcp = true;
        }
        else {
            cursor--;
            prev_iter_jumped_lcp = false;
        }
    }
    
    // add a final sub MEM if there is one and it is not contained in the next parent MEM
    if (sub_mem_end > next_mem_end && sub_mem_end - mem.begin >= min_mem_length && !prev_iter_jumped_lcp) {
        sub_mems_out.push_back(make_pair(MaximalExactMatch(mem.begin, sub_mem_end, range),
                                         vector<size_t>{mems.size() - 1}));
#ifdef debug_mapper
#pragma omp critical
        {
            cerr << "adding sub-MEM ";
            for (auto iter = mem.begin; iter != sub_mem_end; iter++) {
                cerr << *iter;
            }
            cerr << endl;
        }
#endif
        // note: this sub MEM is at the far left side of the parent MEM, so we don't need to
        // check whether earlier MEMs contain it as well
    }
#ifdef debug_mapper
    else {
#pragma omp critical
        {
            cerr << "minimally more frequent MEM is too short ";
            for (auto iter = mem.begin; iter != sub_mem_end; iter++) {
                cerr << *iter;
            }
            cerr << endl;
        }
    }
#endif
}
    
void Mapper::find_sub_mems_fast(vector<MaximalExactMatch>& mems,
                                string::const_iterator next_mem_end,
                                int min_sub_mem_length,
                                vector<pair<MaximalExactMatch, vector<size_t>>>& sub_mems_out) {
    
#ifdef debug_mapper
#pragma omp critical
    cerr << "find_sub_mems_fast: mem ";
    for (auto iter = mems.back().begin; iter != mems.back().end; iter++) {
        cerr << *iter;
    }
    cerr << ", min_sub_mem_length " << min_sub_mem_length << endl;
#endif
    
    // get the most recently added MEM
    MaximalExactMatch& mem = mems.back();
    
    // how many times does the parent MEM occur in the index?
    size_t parent_count = gcsa->count(mem.range);
    
    // the end of the leftmost substring that is at least the minimum length and not contained
    // in the next SMEM
    string::const_iterator probe_string_end = mem.begin + min_sub_mem_length;
    if (probe_string_end <= next_mem_end) {
        probe_string_end = next_mem_end + 1;
    }
    
    while (probe_string_end <= mem.end) {
        
        // locate the probe substring of length equal to the minimum length for a sub-MEM
        // that we are going to test to see if it's inside any sub-MEM
        string::const_iterator probe_string_begin = probe_string_end - min_sub_mem_length;
        
#ifdef debug_mapper
#pragma omp critical
        cerr << "probe string is mem[" << probe_string_begin - mem.begin << ":" << probe_string_end - mem.begin << "] ";
        for (auto iter = probe_string_begin; iter != probe_string_end; iter++) {
            cerr << *iter;
        }
        cerr << endl;
#endif
        
        // set up LF searching
        string::const_iterator cursor = probe_string_end - 1;
        gcsa::range_type range = gcsa::range_type(0, gcsa->size() - 1);
        
        // check if the probe substring is more frequent than the SMEM its contained in
        bool probe_string_more_frequent = true;
        while (cursor >= probe_string_begin) {
            
            range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);
            
            if (gcsa->count(range) <= parent_count) {
                probe_string_more_frequent = false;
                break;
            }
            
            cursor--;
        }
        
        if (probe_string_more_frequent) {
            // this is the prefix of a sub-MEM of length >= the minimum, now we need to
            // find its end using binary search
            
            if (probe_string_end == next_mem_end + 1) {
                // edge case: we arbitrarily moved the probe string to the right to avoid finding
                // sub-MEMs that are contained in the next SMEM, so we don't have the normal guarantee
                // that this match cannot be extended to the left
                // to re-establish this guarantee, we need to walk it out as far as possible before
                // looking for the right end of the sub-MEM
                
                // extend match until beginning of SMEM or until the end of the independent hit
                while (cursor >= mem.begin) {
                    gcsa::range_type last_range = range;
                    range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);
                    
                    if (gcsa->count(range) <= parent_count) {
                        range = last_range;
                        break;
                    }
                    
                    cursor--;
                }
                
                // mark this position as the beginning of the probe substring
                probe_string_begin = cursor + 1;
            }
            
            // inclusive interval that contains the past-the-last index of the sub-MEM
            string::const_iterator left_search_bound = probe_string_end;
            string::const_iterator right_search_bound = mem.end;
            
            // the match range of the longest prefix of the sub-MEM we've found so far (if we initialize
            // it here, the binary search is guaranteed to LF along the full sub-MEM in some iteration)
            gcsa::range_type sub_mem_range = range;
            
            // iterate until inteveral contains only one index
            while (right_search_bound > left_search_bound) {
                
                string::const_iterator middle = left_search_bound + (right_search_bound - left_search_bound + 1) / 2;
                
#ifdef debug_mapper
#pragma omp critical
                cerr << "checking extension mem[" << probe_string_begin - mem.begin << ":" << middle - mem.begin << "] ";
                for (auto iter = probe_string_begin; iter != middle; iter++) {
                    cerr << *iter;
                }
                cerr << endl;
#endif
                
                // set up LF searching
                cursor = middle - 1;
                range = gcsa::range_type(0, gcsa->size() - 1);
                
                // check if there is an independent occurrence of this substring outside of the SMEM
                // TODO: potential optimization: if the range of matches at some index is equal to the
                // range of matches in an already confirmed independent match at the same index, then
                // it will still be so for the rest of the LF queries, so we can bail out of the loop
                // early as a match
                bool contained_in_independent_match = true;
                while (cursor >= probe_string_begin) {
                    
                    range = gcsa->LF(range, gcsa->alpha.char2comp[*cursor]);
                    
                    if (gcsa->count(range) <= parent_count) {
                        contained_in_independent_match = false;
                        break;
                    }
                    
                    cursor--;
                }
                
                if (contained_in_independent_match) {
                    // the end of the sub-MEM must be here or to the right
                    left_search_bound = middle;
                    // update the range of matches (this is the longest match we've verified so far)
                    sub_mem_range = range;
                }
                else {
                    // the end of the sub-MEM must be to the left
                    right_search_bound = middle - 1;
                }
            }

#ifdef debug_mapper
#pragma omp critical
            cerr << "final sub-MEM is mem[" << probe_string_begin - mem.begin << ":" << right_search_bound - mem.begin << "] ";
            for (auto iter = probe_string_begin; iter != right_search_bound; iter++) {
                cerr << *iter;
            }
            cerr << endl;
#endif
            
            // record the sub-MEM
            sub_mems_out.push_back(make_pair(MaximalExactMatch(probe_string_begin, right_search_bound, sub_mem_range),
                                             vector<size_t>{mems.size() - 1}));

            // identify all previous MEMs that also contain this sub-MEM
            for (int64_t i = ((int64_t) mems.size()) - 2; i >= 0; i--) {
                if (probe_string_begin >= mems[i].begin) {
                    // contined in next MEM, add its index to sub MEM's list of parents
                    sub_mems_out.back().second.push_back(i);
                }
                else {
                    // not contained in the next MEM, cannot be contained in earlier ones
                    break;
                }
            }
            
            // the closest possible independent probe string will now occur one position
            // to the right of this sub-MEM
            probe_string_end = right_search_bound + 1;
            
        }
        else {
            // we've found a suffix of the probe substring that is only contained inside the
            // parent SMEM, so we can move far enough to the right that the next probe substring
            // will not contain it
            
            probe_string_end = cursor + min_sub_mem_length + 1;
        }
    }
}
    
void Mapper::first_hit_positions_by_index(MaximalExactMatch& mem,
                                          vector<set<pos_t>>& positions_by_index_out) {
    // find the hit to the first index in the parent MEM's range
    vector<gcsa::node_type> all_first_hits;
    gcsa->locate(mem.range.first, all_first_hits, true, false);
    
    // find where in the graph the first hit of the parent MEM is at each index
    mem_positions_by_index(mem, make_pos_t(all_first_hits[0]), positions_by_index_out);
    
    // in case the first hit occurs in more than one place, accumulate all the hits
    if (all_first_hits.size() > 1) {
        for (size_t i = 1; i < all_first_hits.size(); i++) {
            vector<set<pos_t>> temp_positions_by_index;
            mem_positions_by_index(mem, make_pos_t(all_first_hits[i]),
                                   temp_positions_by_index);
            
            for (size_t i = 0; i < positions_by_index_out.size(); i++) {
                for (const pos_t& pos : temp_positions_by_index[i]) {
                    positions_by_index_out[i].insert(pos);
                }
            }
        }
    }
}

void Mapper::fill_nonredundant_sub_mem_nodes(vector<MaximalExactMatch>& parent_mems,
                                             vector<pair<MaximalExactMatch, vector<size_t> > >::iterator sub_mem_records_begin,
                                             vector<pair<MaximalExactMatch, vector<size_t> > >::iterator sub_mem_records_end) {

    
    // for each MEM, a vector of the positions that it touches at each index along the MEM
    vector<vector<set<pos_t>>> positions_by_index(parent_mems.size());
    
    for (auto iter = sub_mem_records_begin; iter != sub_mem_records_end; iter++) {
        
        pair<MaximalExactMatch, vector<size_t> >& sub_mem_and_parents = *iter;
        
        MaximalExactMatch& sub_mem = sub_mem_and_parents.first;
        vector<size_t>& parent_idxs = sub_mem_and_parents.second;
        
        // how many total hits does each parent MEM have?
        vector<size_t> num_parent_hits;
        // positions their first hits of the parent MEM takes at the start position of the sub-MEM
        vector<set<pos_t>*> first_parent_mem_hit_positions;
        for (size_t parent_idx : parent_idxs) {
            // get the parent MEM
            MaximalExactMatch& parent_mem = parent_mems[parent_idx];
            num_parent_hits.push_back(gcsa->count(parent_mem.range));
            
            if (positions_by_index[parent_idx].empty()) {
                // the parent MEM's positions by index haven't been calculated yet, so do it

                first_hit_positions_by_index(parent_mem, positions_by_index[parent_idx]);

            }
            // the index along the parent MEM that sub MEM starts
            size_t offset = sub_mem.begin - parent_mem.begin;
            first_parent_mem_hit_positions.push_back(&(positions_by_index[parent_idx][offset]));
        }
        
        for (gcsa::size_type i = sub_mem.range.first; i <= sub_mem.range.second; i++) {
            
            // add the locations of the hits, but do not remove duplicates yet
            vector<gcsa::node_type> hits;
            gcsa->locate(i, hits, true, false);
            
            // the number of subsequent hits (including these) that are inside a parent MEM
            size_t parent_hit_jump = 0;
            for (gcsa::node_type node : hits) {
                // look for the hit in each parent MEM
                for (size_t j = 0; j < first_parent_mem_hit_positions.size(); j++) {
                    if (first_parent_mem_hit_positions[j]->count(make_pos_t(node))) {
                        // this hit is also a node on a path of the first occurrence of the parent MEM
                        // that means that this is the first index of the sub-range that corresponds
                        // to the parent MEM's hits
                        
                        // calculate how many more positions to jump
                        parent_hit_jump = num_parent_hits[j];
                        break;
                    }
                }
            }
            
            if (parent_hit_jump > 0) {
                // we're at the start of an interval of parent hits, skip the rest of it
                i += (parent_hit_jump - 1);
            }
            else {
                // these are nonredundant sub MEM hits, add them
                for (gcsa::node_type node : hits) {
                    sub_mem.nodes.push_back(node);
                }
            }
        }
        
        // remove duplicates (copied this functionality from the gcsa locate function, but
        // I don't actually know what it's purpose is)
        gcsa::removeDuplicates(sub_mem.nodes, false);
    }
}

void Mapper::mem_positions_by_index(MaximalExactMatch& mem, pos_t hit_pos,
                                    vector<set<pos_t>>& positions_by_index_out) {
    
    // this is a specialized DFS that keeps track of both the distance along the MEM
    // and the position(s) in the graph in the stack by adding all of the next reachable
    // positions in a layer (i.e. vector) in the stack at the end of each iteration.
    // it also keeps track of whether a position in the graph matched to a position along
    // the MEM can potentially be extended to the full MEM to avoid combinatorially checking
    // all paths through bubbles
    
    size_t mem_length = std::distance(mem.begin, mem.end);
    
    // indicates a pairing of this graph position and this MEM index could be extended to a full match
    positions_by_index_out.clear();
    positions_by_index_out.resize(mem_length);
    
    // indicates a pairing of this graph position and this MEM index could not be extended to a full match
    vector<set<pos_t>> false_pos_by_mem_index(mem_length);
    
    // each record indicates the next edge index to traverse, the number of edges that
    // cannot reach a MEM end, and the positions along each edge out
    vector<pair<pair<size_t, size_t>, vector<pos_t> > > pos_stack;
    pos_stack.push_back(make_pair(make_pair((size_t) 0 , (size_t) 0), vector<pos_t>{hit_pos}));
    
    while (!pos_stack.empty()) {
        size_t mem_idx = pos_stack.size() - 1;

        // which edge are we going to search out of this node next?
        size_t next_idx = pos_stack.back().first.first;
        
        if (next_idx >= pos_stack.back().second.size()) {
            // we have traversed all of the edges out of this position
            
            size_t num_misses = pos_stack.back().first.second;
            bool no_full_matches_possible = (num_misses == pos_stack.back().second.size());
            
            // backtrack to previous node
            pos_stack.pop_back();

            // if necessary, mark the edge into this node as a miss
            if (no_full_matches_possible && !pos_stack.empty()) {
                // all of the edges out failed to reach the end of a MEM, this position is a dead end
                
                // get the position that traversed into the layer we just popped off
                pos_t prev_graph_pos = pos_stack.back().second[pos_stack.back().first.first - 1];
                
                // unlabel this node as a potential hit and instead mark it as a miss
                positions_by_index_out[mem_idx].erase(prev_graph_pos);
                false_pos_by_mem_index[mem_idx].insert(prev_graph_pos);
                
                // increase the count of misses in this layer
                pos_stack.back().first.second++;
            }
            
            // skip the forward search on this iteration
            continue;
        }
        
        // increment to the next edge
        pos_stack.back().first.first++;
        
        pos_t graph_pos = pos_stack.back().second[next_idx];
        
        
        // did we already find a MEM through this position?
        if (positions_by_index_out[mem_idx].count(graph_pos)) {
            // we don't need to check the same MEM suffix again
            continue;
        }
        
        // did we already determine that you can't reach a MEM through this position?
        if (false_pos_by_mem_index[mem_idx].count(graph_pos)) {
            // increase the count of misses in this layer
            pos_stack.back().first.second++;
            
            // we don't need to check the same MEM suffix again
            continue;
        }
        
        // does this graph position match the MEM?
        if (*(mem.begin + mem_idx) != xg_cached_pos_char(graph_pos, xindex, get_node_cache())) {
            // mark this node as a miss
            false_pos_by_mem_index[mem_idx].insert(graph_pos);
            
            // increase the count of misses in this layer
            pos_stack.back().first.second++;
        }
        else {
            // mark this node as a potential hit
            positions_by_index_out[mem_idx].insert(graph_pos);
            
            // are we finished with the MEM?
            if (mem_idx < mem_length - 1) {
                
                // add a layer onto the stack for all of the edges out
                pos_stack.push_back(make_pair(make_pair((size_t) 0 , (size_t) 0),
                                              vector<pos_t>()));
                
                // fill the layer with the next positions
                vector<pos_t>& nexts = pos_stack.back().second;
                for (const pos_t& next_graph_pos : positions_bp_from(graph_pos, 1, false)) {
                    nexts.push_back(next_graph_pos);
                }
            }
        }
    }
}

void Mapper::check_mems(const vector<MaximalExactMatch>& mems) {
    for (auto mem : mems) {
#ifdef debug_mapper
#pragma omp critical
        cerr << "checking MEM: " << mem.sequence() << endl;
#endif
        // TODO: fix this for sub-MEMs
        if (sequence_positions(mem.sequence()) != gcsa_nodes_to_positions(mem.nodes)) {
            cerr << "SMEM failed! " << mem.sequence()
                 << " expected " << sequence_positions(mem.sequence()).size() << " hits "
                 << "but found " << gcsa_nodes_to_positions(mem.nodes).size()
                 << "(aside: this consistency check is broken for sub-MEMs, oops)" << endl;
        }
    }
}

const string mems_to_json(const vector<MaximalExactMatch>& mems) {
    stringstream s;
    s << "[";
    size_t j = 0;
    for (auto& mem : mems) {
        s << "[\"";
        s << mem.sequence();
        s << "\",[";
        size_t i = 0;
        for (auto& node : mem.nodes) {
            s << "\"" << gcsa::Node::decode(node) << "\"";
            if (++i < mem.nodes.size()) s << ",";
        }
        s << "]]";
        if (++j < mems.size()) s << ",";
    }
    s << "]";
    return s.str();
}

char Mapper::pos_char(pos_t pos) {
    return xg_cached_pos_char(pos, xindex, get_node_cache());
}

map<pos_t, char> Mapper::next_pos_chars(pos_t pos) {
    return xg_cached_next_pos_chars(pos, xindex, get_node_cache(), get_edge_cache());
}

int Mapper::graph_distance(pos_t pos1, pos_t pos2, int maximum) {
    return xg_cached_distance(pos1, pos2, maximum, xindex, get_node_cache(), get_edge_cache());
}

int Mapper::approx_position(pos_t pos) {
    // get nodes on the forward strand
    if (is_rev(pos)) {
        pos = reverse(pos, xg_cached_node_length(id(pos), xindex, get_node_cache()));
    }
    return xg_cached_node_start(id(pos), xindex, get_node_start_cache()) + offset(pos);
}

int Mapper::approx_distance(pos_t pos1, pos_t pos2) {
    return approx_position(pos1) - approx_position(pos2);
}

/// returns approximate position of alignnment start in xindex
/// or -1.0 if alignment is unmapped
int Mapper::approx_alignment_position(const Alignment& aln) {
    if (aln.path().mapping_size()) {
        auto& mbeg = aln.path().mapping(0);
        if (mbeg.has_position()) {
            return approx_position(make_pos_t(mbeg.position()));
        } else {
            return -1.0;
        }
    } else {
        return -1.0;
    }
}

/// returns approximate distance between alignment starts
/// or -1.0 if not possible to determine
int Mapper::approx_fragment_length(const Alignment& aln1, const Alignment& aln2) {
    int pos1 = approx_alignment_position(aln1);
    int pos2 = approx_alignment_position(aln2);
    if (pos1 != -1 && pos2 != -1) {
        return abs(pos1 - pos2);
    } else {
        return -1;
    }
}

id_t Mapper::node_approximately_at(int approx_pos) {
    return xindex->node_at_seq_pos(
        min(xindex->seq_length,
            (size_t)max(approx_pos, 1)));
}

set<pos_t> Mapper::positions_bp_from(pos_t pos, int distance, bool rev) {
    return xg_cached_positions_bp_from(pos, distance, rev, xindex, get_node_cache(), get_edge_cache());
}

// use LRU caching to get the most-recent node positions
map<string, vector<size_t> > Mapper::node_positions_in_paths(gcsa::node_type node) {
    auto& pos_cache = get_node_pos_cache();
    auto cached = pos_cache.retrieve(node);
    if(!cached.second) {
        // todo use approximate estimate
        cached.first = xindex->position_in_paths(gcsa::Node::id(node), gcsa::Node::rc(node), gcsa::Node::offset(node));
        pos_cache.put(node, cached.first);
    }
    return cached.first;
}

Alignment Mapper::walk_match(const string& seq, pos_t pos) {
    //cerr << "in walk match with " << seq << " " << seq.size() << " " << pos << endl;
    Alignment aln;
    aln.set_sequence(seq);
    auto alns = walk_match(aln, seq, pos);
    if (!alns.size()) {
        //cerr << "no alignments returned from walk match with " << seq << " " << seq.size() << " " << pos << endl;
        //assert(false);
        return aln;
    }
    aln = alns.front(); // take the first one we found
    //assert(alignment_to_length(aln) == alignment_from_length(aln));
    if (alignment_to_length(aln) != alignment_from_length(aln)
        || alignment_to_length(aln) != seq.size()) {
        //cerr << alignment_to_length(aln) << " is not " << seq.size() << endl;
        //cerr << pb2json(aln) << endl;
        //assert(false);
        aln.clear_path();
    }
#ifdef debug_mapper
    if (debug) {
        cerr << "walk_match result " << pb2json(aln) << endl;
        if (!check_alignment(aln)) {
            cerr << "aln is invalid!" << endl;
            exit(1);
        }
    }
#endif
    return aln;
}

vector<Alignment> Mapper::walk_match(const Alignment& base, const string& seq, pos_t pos) {
    //cerr << "in walk_match " << seq << " from " << pos << " with base " << pb2json(base) << endl;
    // go to the position in the xg index
    // and step in the direction given
    // until we exhaust our sequence
    // or hit another node
    vector<Alignment> alns;
    Alignment aln = base;
    Path& path = *aln.mutable_path();
    Mapping* mapping = path.add_mapping();
    *mapping->mutable_position() = make_position(pos);
#ifdef debug_mapper
#pragma omp critical
    if (debug) cerr << "walking match for seq " << seq << " at position " << pb2json(*mapping) << endl;
#endif
    // get the first node we match
    int total = 0;
    size_t match_len = 0;
    for (size_t i = 0; i < seq.size(); ++i) {
        char c = seq[i];
        //cerr << string(base.path().mapping_size(), ' ') << pos << " @ " << i << " on " << c << endl;
        auto nexts = next_pos_chars(pos);
        // we can have a match on the current node
        if (nexts.size() == 1 && id(nexts.begin()->first) == id(pos)) {
            pos_t npos = nexts.begin()->first;
            // check that the next position would match
            if (i+1 < seq.size()) {
                // we can't step, so we break
                //cerr << "Checking if " << pos_char(npos) << " != " << seq[i+1] << endl;
                if (pos_char(npos) != seq[i+1]) {
#ifdef debug_mapper
#pragma omp critical
                    if (debug) cerr << "MEM does not match position, returning without creating alignment" << endl;
#endif
                    return alns;
                }
            }
            // otherwise we step our counters
            ++match_len;
            ++get_offset(pos);
        } else { // or we go into the next node
            // we must be going into another node
            // emit the mapping for this node
            //cerr << "we are going into a new node" << endl;
            // finish the last node
            {
                // we must have matched / we already checked
                ++match_len;
                Edit* edit = mapping->add_edit();
                edit->set_from_length(match_len);
                edit->set_to_length(match_len);
                // reset our counter
                match_len = 0;
            }
            // find the next node that matches our MEM
            bool got_match = false;
            if (i+1 < seq.size()) {
                //cerr << "nexts @ " << i << " " << nexts.size() << endl;
                for (auto& p : nexts) {
                    //cerr << " next : " << p.first << " " << p.second << " (looking for " << seq[i+1] << ")" << endl;
                    if (p.second == seq[i+1]) {
                        if (!got_match) {
                            pos = p.first;
                            got_match = true;
                        } else {
                            auto v = walk_match(aln, seq.substr(i+1), p.first);
                            if (v.size()) {
                                alns.reserve(alns.size() + distance(v.begin(), v.end()));
                                alns.insert(alns.end(), v.begin(), v.end());
                            }
                        }
                    }
                }
                if (!got_match) {
                    // this matching ends here
                    // and we haven't finished matching
                    // thus this path doesn't contain the match
                    //cerr << "got no match" << endl;
                    return alns;
                }

                // set up a new mapping
                mapping = path.add_mapping();
                *mapping->mutable_position() = make_position(pos);
            } else {
                //cerr << "done!" << endl;
            }
        }
    }
    if (match_len) {
        Edit* edit = mapping->add_edit();
        edit->set_from_length(match_len);
        edit->set_to_length(match_len);
    }
    alns.push_back(aln);
#ifdef debug_mapper
#pragma omp critical
    if (debug) {
        cerr << "walked alignment(s):" << endl;
        for (auto& aln : alns) {
            cerr << pb2json(aln) << endl;
        }
    }
#endif
    //cerr << "returning " << alns.size() << endl;
    return alns;
}

// convert one mem into a set of alignments, one for each exact match
vector<Alignment> Mapper::mem_to_alignments(MaximalExactMatch& mem) {
    vector<Alignment> alns;
    const string seq = mem.sequence();
    for (auto& node : mem.nodes) {
        pos_t pos = make_pos_t(node);
        alns.emplace_back(walk_match(seq, pos));
    }
    return alns;
}

// generate a score from the alignment without realigning
// handles split alignments, where gaps of unknown length are
// by estimating length using the positional paths embedded in the graph
int32_t Mapper::score_alignment(const Alignment& aln) {
    int score = 0;
    int read_offset = 0;
    auto& path = aln.path();
    auto aligner = get_regular_aligner();
    auto qual_adj_aligner = get_qual_adj_aligner();
    for (int i = 0; i < path.mapping_size(); ++i) {
        auto& mapping = path.mapping(i);
        //cerr << "looking at mapping " << pb2json(mapping) << endl;
        for (int j = 0; j < mapping.edit_size(); ++j) {
            auto& edit = mapping.edit(j);
            //cerr << "looking at edit " << pb2json(edit) << endl;
            if (edit_is_match(edit)) {
                if (!aln.quality().empty() && adjust_alignments_for_base_quality) {
                    score += qual_adj_aligner->score_exact_match(
                        aln.sequence().substr(read_offset, edit.to_length()),
                        aln.quality().substr(read_offset, edit.to_length()));
                } else {
                    score += edit.from_length()*aligner->match;
                }
            } else if (edit_is_sub(edit)) {
                score -= aligner->mismatch * edit.sequence().size();
            } else if (edit_is_deletion(edit)) {
                score -= aligner->gap_open + edit.from_length()*aligner->gap_extension;
            } else if (edit_is_insertion(edit)
                       && !((i == 0 && j == 0)
                            || (i == path.mapping_size()-1
                                && j == mapping.edit_size()-1))) {
                // todo how do we score this qual adjusted?
                score -= aligner->gap_open + edit.to_length()*aligner->gap_extension;
            }
            read_offset += edit.to_length();
        }
        // score any intervening gaps in mappings using approximate distances
        if (i+1 < path.mapping_size()) {
            // what is the distance between the last position of this mapping
            // and the first of the next
            Position last_pos = mapping.position();
            last_pos.set_offset(last_pos.offset() + mapping_from_length(mapping));
            Position next_pos = path.mapping(i+1).position();
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "gap: " << make_pos_t(last_pos) << " to " << make_pos_t(next_pos) << endl;
            }
#endif
            int dist = graph_distance(make_pos_t(last_pos), make_pos_t(next_pos), aln.sequence().size());
            if (dist == aln.sequence().size()) {
#ifdef debug_mapper
#pragma omp critical
                {
                    if (debug) cerr << "could not find distance to next target, using approximation" << endl;
                }
#endif
                dist = abs(approx_distance(make_pos_t(last_pos), make_pos_t(next_pos)));
            }
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "distance from " << pb2json(last_pos) << " to " << pb2json(next_pos) << " is " << dist << endl;
            }
#endif
            if (dist > 0) {
                score -= aligner->gap_open + dist * aligner->gap_extension;
            }
        }
    }
#ifdef debug_mapper
#pragma omp critical
    {
        if (debug) cerr << "score from score_alignment " << score << endl;
    }
#endif
    return max(0, score);
}

int32_t Mapper::rescore_without_full_length_bonus(const Alignment& aln) {
    int32_t score = aln.score();
    if (softclip_start(aln) == 0) {
        score -= full_length_alignment_bonus;
    }
    if (softclip_end(aln) == 0) {
        score -= full_length_alignment_bonus;
    }
    return score;
}

// make a perfect-match alignment out of a vector of MEMs which each have only one recorded hit
// use the base alignment sequence (which the SMEMs relate to) to fill in the gaps
Alignment Mapper::mems_to_alignment(const Alignment& aln, vector<MaximalExactMatch>& mems) {
    // base case--- empty alignment
    if (mems.empty()) {
        Alignment aln; return aln;
    }
    vector<Alignment> alns;
    // get reference to the start and end of the sequences
    string::const_iterator seq_begin = aln.sequence().begin();
    string::const_iterator seq_end = aln.sequence().end();
    // we use this to track where we need to add sequence
    string::const_iterator last_end = seq_begin;
    for (int i = 0; i < mems.size(); ++i) {
        auto& mem = mems.at(i);
        //cerr << "looking at " << mem.sequence() << endl;
        // this mem is contained in the last
        if (mem.end <= last_end) {
            continue;
        }
        // handle unaligned portion between here and the last SMEM or start of read
        if (mem.begin > last_end) {
            alns.emplace_back();
            alns.back().set_sequence(aln.sequence().substr(last_end - seq_begin, mem.begin - last_end));
        }
        Alignment aln = mem_to_alignment(mem);
        // find and trim overlap with previous
        if (i > 0) {
            // use the end of the last mem we touched (we may have skipped several)
            int overlap = last_end - mem.begin;
            if (overlap > 0) {
                aln = strip_from_start(aln, overlap);
            }
        }
        alns.push_back(aln);
        last_end = mem.end;
    }
    // handle unaligned portion at end of read
    int start = last_end - seq_begin;
    int length = seq_end - (seq_begin + start);
    
    alns.emplace_back();
    alns.back().set_sequence(aln.sequence().substr(start, length));

    auto alnm = merge_alignments(alns);
    *alnm.mutable_quality() = aln.quality();
    return alnm;
}

// convert one mem into an alignment; validates that only one node is given
Alignment Mapper::mem_to_alignment(MaximalExactMatch& mem) {
    const string seq = mem.sequence();
    if (mem.nodes.size() > 1) {
        cerr << "[vg::Mapper] warning: generating first alignment from MEM with multiple recorded hits" << endl;
    }
    auto& node = mem.nodes.front();
    pos_t pos = make_pos_t(node);
    return walk_match(seq, pos);
}

// transform the path into a path relative to another path (defined by path_name)
// source -> surjection (in path_name coordinate space)
// the product is equivalent to a pairwise alignment between this path and the other

// new approach
// get path sequence
// get graph component overlapping path
// removing elements which aren't in the path of interest
// realign to this graph
// cross fingers

Alignment Mapper::surject_alignment(const Alignment& source,
                                    set<string>& path_names,
                                    string& path_name,
                                    int64_t& path_pos,
                                    bool& path_reverse,
                                    int window) {

    Alignment surjection = source;
    surjection.clear_mapping_quality();
    surjection.clear_score();
    surjection.clear_identity();
    surjection.clear_path();

    // get start and end nodes in path
    // get range between +/- window
    if (!source.has_path() || source.path().mapping_size() == 0) {
#ifdef debug

#pragma omp critical (cerr)
        cerr << "Alignment " << source.name() << " is unmapped and cannot be surjected" << endl;

#endif
        return surjection;
    }

    set<id_t> nodes;
    for (int i = 0; i < source.path().mapping_size(); ++ i) {
        nodes.insert(source.path().mapping(i).position().node_id());
    }
    VG graph;
    for (auto& node : nodes) {
        *graph.graph.add_node() = xindex->node(node);
    }
    xindex->expand_context(graph.graph, context_depth, true); // get connected edges and path
    graph.paths.append(graph.graph);
    graph.rebuild_indexes();

    set<string> kept_paths;
    graph.keep_paths(path_names, kept_paths);

    // We need this for inverting mappings to the correct strand
    function<int64_t(id_t)> node_length = [&graph](id_t node) {
        return graph.get_node(node)->sequence().size();
    };
    
    // What is our alignment to surject spelled the other way around? We can't
    // just use the normal alignment RC function because the mappings reference
    // nonexistent nodes.
    // Make sure to copy all the things about the alignment (name, etc.)

    Alignment surjection_rc = surjection;
    surjection_rc.set_sequence(reverse_complement(surjection.sequence()));
    
    // Align the old alignment to the graph in both orientations. Apparently
    // align only does a single oriantation, and we have no idea, even looking
    // at the mappings, which of the orientations will correspond to the one the
    // alignment is actually in.

    auto surjection_forward = align_to_graph(surjection, graph, max_query_graph_ratio);
    auto surjection_reverse = align_to_graph(surjection_rc, graph, max_query_graph_ratio);

#ifdef debug
#pragma omp critical (cerr)
    cerr << surjection.name() << " " << surjection_forward.score() << " forward score, " << surjection_reverse.score() << " reverse score" << endl;
#endif
    
    if(surjection_reverse.score() > surjection_forward.score()) {
        // Even if we have to surject backwards, we have to send the same string out as we got in.
        surjection = reverse_complement_alignment(surjection_reverse, node_length);
    } else {
        surjection = surjection_forward;
    }
    
    
#ifdef debug

#pragma omp critical (cerr)
        cerr << surjection.path().mapping_size() << " mappings, " << kept_paths.size() << " paths" << endl;

#endif

    if (surjection.path().mapping_size() > 0 && kept_paths.size() == 1) {
        // determine the paths of the node we mapped into
        //  ... get the id of the first node, get the paths of it
        assert(kept_paths.size() == 1);
        path_name = *kept_paths.begin();

        int64_t path_id = xindex->path_rank(path_name);
        auto& first_pos = surjection.path().mapping(0).position();
        int64_t hit_id = surjection.path().mapping(0).position().node_id();
        bool hit_backward = surjection.path().mapping(0).position().is_reverse();
        // we pick up positional information using the index

        auto path_posns = xindex->position_in_path(hit_id, path_name);
        if (path_posns.size() > 1) {
            cerr << "[vg map] surject_alignment: warning, multiple positions for node " << hit_id << " in " << path_name << " but will use only first: " << path_posns.front() << endl;
        } else if (path_posns.size() == 0) {
            cerr << "[vg map] surject_alignment: error, no positions for alignment " << source.name() << endl;
            exit(1);
        }

        // if we are reversed
        path_pos = path_posns.front();
        bool reversed_path = xindex->mapping_at_path_position(path_name, path_pos).position().is_reverse();
        if (reversed_path) {
            // if we got the start of the node position relative to the path
            // we need to offset to make thinsg right
            // but which direction
            if (hit_backward) {
                path_pos = path_posns.front() + first_pos.offset();
            } else {
                auto pos = reverse_complement_alignment(surjection, node_length).path().mapping(0).position();
                path_pos = xindex->position_in_path(pos.node_id(), path_name).front() + pos.offset();
            }
            path_reverse = !hit_backward;
        } else {
            if (!hit_backward) {
                path_pos = path_posns.front() + first_pos.offset();
            } else {
                auto pos = reverse_complement_alignment(surjection, node_length).path().mapping(0).position();
                path_pos = xindex->position_in_path(pos.node_id(), path_name).front() + pos.offset();
            }
            path_reverse = hit_backward;
        }

    } else {

        surjection = source;
#ifdef debug

#pragma omp critical (cerr)
        cerr << "Alignment " << source.name() << " did not align to the surjection subgraph" << endl;

#endif

    }

    return surjection;
}

const int balanced_stride(int read_length, int kmer_size, int stride) {
    double r = read_length;
    double k = kmer_size;
    double j = stride;
    int i = (r > j) ? round((r-k)/round((r-k)/j)) : j;
    return max(1, i);
}

const vector<string> balanced_kmers(const string& seq, const int kmer_size, const int stride) {
    // choose the closest stride that will generate balanced kmers
    vector<string> kmers;
    int b = balanced_stride(seq.size(), kmer_size, stride);
    if (!seq.empty()) {
        for (int i = 0; i+kmer_size <= seq.size(); i+=b) {
            kmers.push_back(seq.substr(i,kmer_size));
        }
    }
    return kmers;
}

bool operator==(const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
    return m1.begin == m2.begin && m1.end == m2.end && m1.nodes == m2.nodes;
}

bool operator<(const MaximalExactMatch& m1, const MaximalExactMatch& m2) {
    return m1.begin < m2.begin && m1.end < m2.end && m1.nodes < m2.nodes;
}

MEMChainModel::MEMChainModel(
    const vector<size_t>& aln_lengths,
    const vector<vector<MaximalExactMatch> >& matches,
    Mapper* mapper,
    const function<double(const MaximalExactMatch&, const MaximalExactMatch&)>& transition_weight,
    int band_width,
    int position_depth,
    int max_connections) {
    // store the MEMs in the model
    int frag_n = 0;
    for (auto& fragment : matches) {
        ++frag_n;
        for (auto& mem : fragment) {
            // copy the MEM for each specific hit in the base graph
            // and add it in as a vertex
            for (auto& node : mem.nodes) {
                //model.emplace_back();
                //auto m = model.back();
                MEMChainModelVertex m;
                m.mem = mem;
                m.weight = mem.length();
                m.prev = nullptr;
                m.score = 0;
                m.approx_position = mapper->approx_position(make_pos_t(node));
                m.mem.nodes.clear();
                m.mem.nodes.push_back(node);
                m.mem.fragment = frag_n;
                m.mem.match_count = mem.match_count;
                model.push_back(m);
            }
        }
    }
    // index the model with the positions
    for (vector<MEMChainModelVertex>::iterator v = model.begin(); v != model.end(); ++v) {
        approx_positions[v->approx_position].push_back(v);
    }
    // sort the vertexes at each approx position by their matches and trim
    for (auto& pos : approx_positions) {
        std::sort(pos.second.begin(), pos.second.end(), [](const vector<MEMChainModelVertex>::iterator& v1,
                                                           const vector<MEMChainModelVertex>::iterator& v2) {
                      return v1->mem.length() > v2->mem.length();
                  });
        if (pos.second.size() > position_depth) {
            for (int i = position_depth; i < pos.second.size(); ++i) {
                redundant_vertexes.insert(pos.second[i]);
            }
        }
        pos.second.resize(min(pos.second.size(), (size_t)position_depth));
    }
    // for each vertex merge if we go equivalently forward in the positional space and forward in the read to the next position
    // scan forward
    for (map<int, vector<vector<MEMChainModelVertex>::iterator> >::iterator p = approx_positions.begin();
         p != approx_positions.end(); ++p) {
        for (auto& v1 : p->second) {
            if (redundant_vertexes.count(v1)) continue;
            auto q = p;
            while (++q != approx_positions.end() && abs(p->first - q->first) < band_width) {
                for (auto& v2 : q->second) {
                    if (redundant_vertexes.count(v2)) continue;
                    if (mems_overlap(v1->mem, v2->mem)
                        && abs(v2->mem.begin - v1->mem.begin) == abs(q->first - p->first)) {
                        if (v2->mem.length() < v1->mem.length()) {
                            redundant_vertexes.insert(v2);
                            if (v2->mem.end > v1->mem.end) {
                                v1->weight += v2->mem.end - v1->mem.end;
                            }
                        }
                    }
                }
            }
        }
    }
    // scan reverse
    for (map<int, vector<vector<MEMChainModelVertex>::iterator> >::reverse_iterator p = approx_positions.rbegin();
         p != approx_positions.rend(); ++p) {
        for (auto& v1 : p->second) {
            if (redundant_vertexes.count(v1)) continue;
            auto q = p;
            while (++q != approx_positions.rend() && abs(p->first - q->first) < band_width) {
                for (auto& v2 : q->second) {
                    if (redundant_vertexes.count(v2)) continue;
                    if (mems_overlap(v1->mem, v2->mem)
                        && abs(v2->mem.begin - v1->mem.begin) == abs(p->first - q->first)) {
                        if (v2->mem.length() < v1->mem.length()) {
                            redundant_vertexes.insert(v2);
                            if (v2->mem.end > v1->mem.end) {
                                v1->weight += v2->mem.end - v1->mem.end;
                            }
                        }
                    }
                }
            }
        }
    }
    // now build up the model using the positional bandwidth
    for (map<int, vector<vector<MEMChainModelVertex>::iterator> >::iterator p = approx_positions.begin();
         p != approx_positions.end(); ++p) {
        // look bandwidth before and bandwidth after in the approx positions
        // after
        for (auto& v1 : p->second) {
            if (redundant_vertexes.count(v1)) continue;
            auto q = p;
            while (++q != approx_positions.end() && abs(p->first - q->first) < band_width) {
                for (auto& v2 : q->second) {
                    if (redundant_vertexes.count(v2)) continue;
                    // if this is an allowable transition, run the weighting function on it
                    if (v1->next_cost.size() < max_connections
                        && v2->prev_cost.size() < max_connections) {
                        if (v1->mem.fragment < v2->mem.fragment
                            || v1->mem.fragment == v2->mem.fragment && v1->mem.begin < v2->mem.begin) {
                            double weight = transition_weight(v1->mem, v2->mem);
                            if (weight > -std::numeric_limits<double>::max()) {
                                v1->next_cost.push_back(make_pair(&*v2, weight));
                                v2->prev_cost.push_back(make_pair(&*v1, weight));
                            }
                        } else if (v1->mem.fragment > v2->mem.fragment
                                   || v1->mem.fragment == v2->mem.fragment && v1->mem.begin > v2->mem.begin) {
                            double weight = transition_weight(v2->mem, v1->mem);
                            if (weight > -std::numeric_limits<double>::max()) {
                                v2->next_cost.push_back(make_pair(&*v1, weight));
                                v1->prev_cost.push_back(make_pair(&*v2, weight));
                            }
                        }
                    }
                }
            }
        }
    }
}

void MEMChainModel::score(const set<MEMChainModelVertex*>& exclude) {
    // propagate the scores in the model
    for (auto& m : model) {
        // score is equal to the max inbound + mem.weight
        if (exclude.count(&m)) continue; // skip if vertex was whole cluster
        m.score = m.weight;
        for (auto& p : m.prev_cost) {
            if (p.first == nullptr) continue; // this transition is masked out
            double proposal = m.weight + p.second + p.first->score;
            if (proposal > m.score) {
                m.prev = p.first;
                m.score = proposal;
            }
        }
    }
}

MEMChainModelVertex* MEMChainModel::max_vertex(void) {
    MEMChainModelVertex* maxv = nullptr;
    for (auto& m : model) {
        if (maxv == nullptr || m.score > maxv->score) {
            maxv = &m;
        }
    }
    return maxv;
}

void MEMChainModel::clear_scores(void) {
    for (auto& m : model) {
        m.score = 0;
        m.prev = nullptr;
    }
}

vector<vector<MaximalExactMatch> > MEMChainModel::traceback(int alt_alns, bool paired, bool debug) {
    vector<vector<MaximalExactMatch> > traces;
    traces.reserve(alt_alns); // avoid reallocs so we can refer to pointers to the traces
    set<MEMChainModelVertex*> exclude;
    for (auto& v : redundant_vertexes) exclude.insert(&*v);
    for (int i = 0; i < alt_alns; ++i) {
        // score the model, accounting for excluded traces
        clear_scores();
        score(exclude);
#ifdef debug_mapper
#pragma omp critical
        {
            if (debug) {
                cerr << "MEMChainModel::traceback " << i << endl;
                display(cerr);
            }
        }
#endif
        vector<MEMChainModelVertex*> vertex_trace;
        {
            // find the maximum score
            auto* vertex = max_vertex();
            // check if we've exhausted our MEMs
            if (vertex == nullptr || vertex->score == 0) break;
#ifdef debug_mapper
#pragma omp critical
            {
                if (debug) cerr << "maximum score " << vertex->mem.sequence() << " " << vertex << ":" << vertex->score << endl;
            }
#endif
            // make trace
            while (vertex != nullptr) {
                vertex_trace.push_back(vertex);
                if (vertex->prev != nullptr) {
                    vertex = vertex->prev;
                } else {
                    break;
                }
            }
        }
        // if we have a singular match or reads are not paired, record not to use it again
        if (paired && vertex_trace.size() == 1) {
            exclude.insert(vertex_trace.front());
        }
        // fill this out when we're paired to help mask out in-fragment transitions
        set<MEMChainModelVertex*> chain_members;
        if (paired) for (auto v : vertex_trace) chain_members.insert(v);
        traces.emplace_back();
        auto& mem_trace = traces.back();
        for (auto v = vertex_trace.rbegin(); v != vertex_trace.rend(); ++v) {
            auto& vertex = **v;
            if (!paired) exclude.insert(&vertex);
            if (v != vertex_trace.rbegin()) {
                auto y = v - 1;
                MEMChainModelVertex* prev = *y;
                // mask out used transitions
                for (auto& p : vertex.prev_cost) {
                    if (p.first == prev) {
                        p.first = nullptr;
                    } else if (paired && p.first != nullptr
                               && p.first->mem.fragment != vertex.mem.fragment
                               && chain_members.count(p.first)) {
                        p.first = nullptr;
                    }
                }
            }
            mem_trace.push_back(vertex.mem);
        }
    }
    return traces;
}

// show model
void MEMChainModel::display(ostream& out) {
    for (auto& vertex : model) {
        out << vertex.mem.sequence() << ":" << vertex.mem.fragment << " " << &vertex << ":" << vertex.score << "@";
        for (auto& node : vertex.mem.nodes) {
            id_t id = gcsa::Node::id(node);
            size_t offset = gcsa::Node::offset(node);
            bool is_rev = gcsa::Node::rc(node);
            out << id << (is_rev ? "-" : "+") << ":" << offset << " ";
        }
        out << "prev: ";
        for (auto& p : vertex.prev_cost) {
            auto& next = p.first;
            if (p.first == nullptr) continue;
            out << p.first << ":" << p.second << "@";
            for (auto& node : next->mem.nodes) {
                id_t id = gcsa::Node::id(node);
                size_t offset = gcsa::Node::offset(node);
                bool is_rev = gcsa::Node::rc(node);
                out << id << (is_rev ? "-" : "+") << ":" << offset << " ";
            }
            out << " ; ";
        }
        out << " next: ";
        for (auto& p : vertex.next_cost) {
            auto& next = p.first;
            if (p.first == nullptr) continue;
            out << p.first << ":" << p.second << "@";
            for (auto& node : next->mem.nodes) {
                id_t id = gcsa::Node::id(node);
                size_t offset = gcsa::Node::offset(node);
                bool is_rev = gcsa::Node::rc(node);
                out << id << (is_rev ? "-" : "+") << ":" << offset << " ";
            }
            out << " ; ";
        }
        out << endl;
    }
}

// construct the sequence of the MEM; useful in debugging
string MaximalExactMatch::sequence(void) const {
    string seq; //seq.resize(end-begin);
    string::const_iterator c = begin;
    while (c != end) seq += *c++;
    return seq;
}
    
// length of the MEM
int MaximalExactMatch::length(void) const {
    return end - begin;
}

// uses an xgindex to fill out the MEM positions
void MaximalExactMatch::fill_positions(Mapper* mapper) {
    for (auto& node : nodes) {
        positions = mapper->node_positions_in_paths(gcsa::Node::encode(gcsa::Node::id(node), 0, gcsa::Node::rc(node)));
    }
}

// counts Ns in the MEM
size_t MaximalExactMatch::count_Ns(void) const {
    return std::count(begin, end, 'N');
}

ostream& operator<<(ostream& out, const MaximalExactMatch& mem) {
    size_t len = mem.begin - mem.end;
    out << mem.sequence() << ":";
    for (auto& node : mem.nodes) {
        id_t id = gcsa::Node::id(node);
        size_t offset = gcsa::Node::offset(node);
        bool is_rev = gcsa::Node::rc(node);
        out << id << (is_rev ? "-" : "+") << ":" << offset << ",";
    }
    return out;
}
    

}
