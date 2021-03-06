//
//  phased_genome.cpp
//
//  Unit tests for PhasedGenome object
//

#include <stdio.h>
#include <iostream>
#include <list>
#include "json2pb.h"
#include "vg.pb.h"
#include "catch.hpp"
#include "utility.hpp"
#include "phased_genome.hpp"
#include "multipath_alignment.hpp"
#include "nodetraversal.hpp"
#include "vg.hpp"
#include "genotypekit.hpp"

namespace vg {
    namespace unittest {
        
        TEST_CASE( "PhasedGenome can be built correctly",
                  "[phasing][mcmc]" ) {
            
            SECTION( "PhasedGenome constructor runs") {
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
            }
            
            SECTION( "PhasedGenome can add haplotypes") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // construct haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
            }
            
            SECTION( "PhasedGenome can iterate through haplotypes") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // construct haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                auto iter = genome.begin(0);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n2, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(0));
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n3, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
                
            }
            
        }
        
        // TODO: temporarily disabling these tests because they're failing, but they're for an
        // experimental feature that isn't included in the VG paper
        
        TEST_CASE("PhasedGenome can be indexed and edited correctly",
                  "[phasing][mcmc]" ) {
        
            SECTION( "PhasedGenome can build site indices with simple sites") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // construct haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
            }
            
            SECTION( "PhasedGenome can build indices with nested sites") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n6);
                graph.create_edge(n2, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n5);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // construct haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n2));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n5));
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
            }
            
            SECTION( "PhasedGenome can set a simple allele") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // construct haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
                list<NodeTraversal> allele;
                allele.push_back(NodeTraversal(n2));
                
                auto iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n3, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
                
                // find the site we want to switch
                for (const Snarl* site : snarl_manager.top_level_snarls()) {
                    if (site->start().node_id() == n1->id() || site->end().node_id() == n1->id()) {
                        // set the allele
                        genome.set_allele(*site, allele.begin(), allele.end(), 1);
                        break;
                    }
                }
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n2, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
            }
            
            SECTION( "PhasedGenome can swap a simple allele between chromosomes") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // construct haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
                auto iter = genome.begin(0);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n2, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(0));
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n3, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
                
                // find the site we want to switch
                for (const Snarl* site : snarl_manager.top_level_snarls()) {
                    if (site->start().node_id() == n1->id() || site->end().node_id() == n1->id()) {
                        // swap the allele
                        genome.swap_alleles(*site, 0, 1);
                        break;
                    }
                }
                
                iter = genome.begin(0);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n3, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(0));
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n2, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
            }
            
            SECTION( "PhasedGenome can perform successive allele swaps") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // add haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
                // find the site we want to switch
                for (const Snarl* site : snarl_manager.top_level_snarls()) {
                    if (site->start().node_id() == n1->id() || site->end().node_id() == n1->id()) {
                        // swap the allele twice
                        genome.swap_alleles(*site, 0, 1);
                        genome.swap_alleles(*site, 0, 1);
                        break;
                    }
                }
                
                auto iter = genome.begin(0);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n2, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(0));
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n3, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));

            }
            
            SECTION( "PhasedGenome can perform successive allele swaps involving a deletion") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n3);
                graph.create_edge(n1, n4);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n4);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                // segregating inversion
                graph.create_edge(n4, n5, false, true);
                graph.create_edge(n5, n6, true, false);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n4));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n3));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5, true)); // reversed
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
                // find the site we want to switch
                for (const Snarl* site : snarl_manager.top_level_snarls()) {
                    if (site->start().node_id() == n1->id() || site->end().node_id() == n1->id()) {
                        // swap the allele twice
                        genome.swap_alleles(*site, 0, 1);
                        genome.swap_alleles(*site, 0, 1);
                        break;
                    }
                }
                
                auto iter = genome.begin(0);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(0));
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n3, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, true));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
                
            }
            
            SECTION( "PhasedGenome can perform successive allele edits in a nested allele") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("T");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("CTGA");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n6);
                graph.create_edge(n2, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n5);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // add haplotypes
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n3));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
                list<NodeTraversal> allele;
                allele.push_back(NodeTraversal(n4));
                
                // find the site we want to switch
                const Snarl* site = snarl_manager.top_level_snarls()[0];
                const Snarl* subsite = snarl_manager.children_of(site)[0];
                
                genome.swap_alleles(*site, 0, 1);
                genome.set_allele(*subsite, allele.begin(), allele.end(), 1);
                
                auto iter = genome.begin(0);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(0));
                
                iter = genome.begin(1);
                REQUIRE((*iter) == NodeTraversal(n1, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n2, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n4, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n5, false));
                iter++;
                REQUIRE((*iter) == NodeTraversal(n6, false));
                iter++;
                REQUIRE(iter == genome.end(1));
            }
        }
        
        
        TEST_CASE("PhasedGenome can perform computations on multipath alignments",
                  "[phasing][mcmc]" ) {
            
            SECTION( "PhasedGenome can compute the score of a restricted multipath alignment on one haplotype") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("CTGT");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("A");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n6);
                graph.create_edge(n2, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n5);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                const Snarl* site = snarl_manager.top_level_snarls()[0];
                const Snarl* subsite = snarl_manager.children_of(site)[0];
                
                // add haplotypes
                
                list<NodeTraversal> haplotype_1;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n3));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                
                // index sites
                
                genome.build_indices();
                
                // construct a multipath alignment
                
                MultipathAlignment multipath_aln;
                multipath_aln.set_sequence("CACTGTATTGCGGATA");
                
                // add subpaths
                Subpath* subpath0 = multipath_aln.add_subpath();
                Subpath* subpath1 = multipath_aln.add_subpath();
                Subpath* subpath2 = multipath_aln.add_subpath();
                Subpath* subpath3 = multipath_aln.add_subpath();
                
                // set edges between subpaths
                subpath0->add_next(1);
                subpath0->add_next(2);
                subpath1->add_next(3);
                subpath2->add_next(3);
                
                // designate mappings and scores
                Mapping* mapping0 = subpath0->mutable_path()->add_mapping();
                mapping0->mutable_position()->set_node_id(1);
                mapping0->mutable_position()->set_offset(1);
                Edit* edit00 = mapping0->add_edit();
                edit00->set_from_length(2);
                edit00->set_to_length(2);
                
                Mapping* mapping1 = subpath0->mutable_path()->add_mapping();
                mapping1->mutable_position()->set_node_id(2);
                Edit* edit10 = mapping1->add_edit();
                edit10->set_from_length(4);
                edit10->set_to_length(4);
                
                subpath0->set_score(6);
                
                Mapping* mapping2 = subpath1->mutable_path()->add_mapping();
                mapping2->mutable_position()->set_node_id(3);
                Edit* edit20 = mapping2->add_edit();
                edit20->set_from_length(1);
                edit20->set_to_length(1);
                edit20->set_sequence("A");
                
                subpath1->set_score(-4);
                
                Mapping* mapping3 = subpath2->mutable_path()->add_mapping();
                mapping3->mutable_position()->set_node_id(4);
                Edit* edit30 = mapping3->add_edit();
                edit30->set_from_length(1);
                edit30->set_to_length(1);
                
                subpath2->set_score(1);
                
                Mapping* mapping4 = subpath3->mutable_path()->add_mapping();
                mapping4->mutable_position()->set_node_id(5);
                Edit* edit40 = mapping4->add_edit();
                edit40->set_from_length(3);
                edit40->set_to_length(3);
                
                Mapping* mapping5 = subpath3->mutable_path()->add_mapping();
                mapping5->mutable_position()->set_node_id(6);
                Edit* edit50 = mapping5->add_edit();
                edit50->set_from_length(6);
                edit50->set_to_length(6);
                
                subpath3->set_score(9);
                
                identify_start_subpaths(multipath_aln);
                
                REQUIRE( genome.optimal_score_on_genome(multipath_aln, graph) == 6 - 4 + 9 );
                
                list<NodeTraversal> allele;
                allele.push_back(NodeTraversal(n4));
                
                genome.set_allele(*subsite, allele.begin(), allele.end(), 0);
                
                REQUIRE( genome.optimal_score_on_genome(multipath_aln, graph) == 6 + 1 + 9 );
            }
            
            
            
            SECTION( "PhasedGenome can compute the score of a restricted multipath alignment on two haplotype") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("CTGT");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("A");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n6);
                graph.create_edge(n2, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n3, n5);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                
                // construct phased genome
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                const Snarl* site = snarl_manager.top_level_snarls()[0];
                const Snarl* subsite = snarl_manager.children_of(site)[0];
                
                list<NodeTraversal> haplotype_1;
                list<NodeTraversal> haplotype_2;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n2));
                haplotype_1.push_back(NodeTraversal(n3));
                haplotype_1.push_back(NodeTraversal(n5));
                haplotype_1.push_back(NodeTraversal(n6));
                
                haplotype_2.push_back(NodeTraversal(n1));
                haplotype_2.push_back(NodeTraversal(n2));
                haplotype_2.push_back(NodeTraversal(n4));
                haplotype_2.push_back(NodeTraversal(n5));
                haplotype_2.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                genome.add_haplotype(haplotype_2.begin(), haplotype_2.end());
                
                // index sites
                
                genome.build_indices();
                
                // construct a multipath alignment
                
                MultipathAlignment multipath_aln;
                multipath_aln.set_sequence("CACTGTATTGCGGATA");
                
                // add subpaths
                Subpath* subpath0 = multipath_aln.add_subpath();
                Subpath* subpath1 = multipath_aln.add_subpath();
                Subpath* subpath2 = multipath_aln.add_subpath();
                Subpath* subpath3 = multipath_aln.add_subpath();
                
                // set edges between subpaths
                subpath0->add_next(1);
                subpath0->add_next(2);
                subpath1->add_next(3);
                subpath2->add_next(3);
                
                // designate mappings and scores
                Mapping* mapping0 = subpath0->mutable_path()->add_mapping();
                mapping0->mutable_position()->set_node_id(1);
                mapping0->mutable_position()->set_offset(1);
                Edit* edit00 = mapping0->add_edit();
                edit00->set_from_length(2);
                edit00->set_to_length(2);
                
                Mapping* mapping1 = subpath0->mutable_path()->add_mapping();
                mapping1->mutable_position()->set_node_id(2);
                Edit* edit10 = mapping1->add_edit();
                edit10->set_from_length(4);
                edit10->set_to_length(4);
                
                subpath0->set_score(6);
                
                Mapping* mapping2 = subpath1->mutable_path()->add_mapping();
                mapping2->mutable_position()->set_node_id(3);
                Edit* edit20 = mapping2->add_edit();
                edit20->set_from_length(1);
                edit20->set_to_length(1);
                edit20->set_sequence("A");
                
                subpath1->set_score(-4);
                
                Mapping* mapping3 = subpath2->mutable_path()->add_mapping();
                mapping3->mutable_position()->set_node_id(4);
                Edit* edit30 = mapping3->add_edit();
                edit30->set_from_length(1);
                edit30->set_to_length(1);
                
                subpath2->set_score(1);
                
                Mapping* mapping4 = subpath3->mutable_path()->add_mapping();
                mapping4->mutable_position()->set_node_id(5);
                Edit* edit40 = mapping4->add_edit();
                edit40->set_from_length(3);
                edit40->set_to_length(3);
                
                Mapping* mapping5 = subpath3->mutable_path()->add_mapping();
                mapping5->mutable_position()->set_node_id(6);
                Edit* edit50 = mapping5->add_edit();
                edit50->set_from_length(6);
                edit50->set_to_length(6);
                
                subpath3->set_score(9);
                
                identify_start_subpaths(multipath_aln);
                
                REQUIRE( genome.optimal_score_on_genome(multipath_aln, graph) == 6 + 1 + 9 );
            }
            
            SECTION( "PhasedGenome can compute the score of a restricted multipath alignment on the reverse strand") {
                
                // construct graph
                
                VG graph;
                
                Node* n1 = graph.create_node("GCA");
                Node* n2 = graph.create_node("CTGT");
                Node* n3 = graph.create_node("G");
                Node* n4 = graph.create_node("A");
                Node* n5 = graph.create_node("TTG");
                Node* n6 = graph.create_node("CGGATA");
                
                graph.create_edge(n1, n2);
                graph.create_edge(n1, n5, false, true);
                graph.create_edge(n2, n3);
                graph.create_edge(n2, n4);
                graph.create_edge(n2, n6, true, false);
                graph.create_edge(n3, n5);
                graph.create_edge(n4, n5);
                graph.create_edge(n5, n6);
                
                CactusUltrabubbleFinder bubble_finder(graph, "");
                SnarlManager snarl_manager = bubble_finder.find_snarls();
                
                PhasedGenome genome = PhasedGenome(snarl_manager);
                
                // constuct haplotypes
                
                list<NodeTraversal> haplotype_1;
                
                haplotype_1.push_back(NodeTraversal(n1));
                haplotype_1.push_back(NodeTraversal(n5, true));
                haplotype_1.push_back(NodeTraversal(n3, true));
                haplotype_1.push_back(NodeTraversal(n2, true));
                haplotype_1.push_back(NodeTraversal(n6));
                
                genome.add_haplotype(haplotype_1.begin(), haplotype_1.end());
                
                // index sites
                
                genome.build_indices();
                
                // construct a multipath alignment
                
                MultipathAlignment multipath_aln;
                multipath_aln.set_sequence("CCGCTGTATTGTGC");
                
                // add subpaths
                Subpath* subpath0 = multipath_aln.add_subpath();
                Subpath* subpath1 = multipath_aln.add_subpath();
                Subpath* subpath2 = multipath_aln.add_subpath();
                Subpath* subpath3 = multipath_aln.add_subpath();
                
                // set edges between subpaths
                subpath0->add_next(1);
                subpath0->add_next(2);
                subpath1->add_next(3);
                subpath2->add_next(3);
                
                // designate mappings and scores
                Mapping* mapping0 = subpath0->mutable_path()->add_mapping();
                mapping0->mutable_position()->set_node_id(6);
                mapping0->mutable_position()->set_offset(3);
                mapping0->mutable_position()->set_is_reverse(true);
                Edit* edit00 = mapping0->add_edit();
                edit00->set_from_length(3);
                edit00->set_to_length(3);
                
                Mapping* mapping1 = subpath0->mutable_path()->add_mapping();
                mapping1->mutable_position()->set_node_id(2);
                Edit* edit10 = mapping1->add_edit();
                edit10->set_from_length(4);
                edit10->set_to_length(4);
                
                subpath0->set_score(7);
                
                Mapping* mapping2 = subpath1->mutable_path()->add_mapping();
                mapping2->mutable_position()->set_node_id(3);
                Edit* edit20 = mapping2->add_edit();
                edit20->set_from_length(1);
                edit20->set_to_length(1);
                edit20->set_sequence("A");
                
                subpath1->set_score(-4);
                
                Mapping* mapping3 = subpath2->mutable_path()->add_mapping();
                mapping3->mutable_position()->set_node_id(4);
                Edit* edit30 = mapping3->add_edit();
                edit30->set_from_length(1);
                edit30->set_to_length(1);
                
                subpath2->set_score(1);
                
                Mapping* mapping4 = subpath3->mutable_path()->add_mapping();
                mapping4->mutable_position()->set_node_id(5);
                Edit* edit40 = mapping4->add_edit();
                edit40->set_from_length(3);
                edit40->set_to_length(3);
                
                Mapping* mapping5 = subpath3->mutable_path()->add_mapping();
                mapping5->mutable_position()->set_node_id(1);
                mapping5->mutable_position()->set_is_reverse(true);
                Edit* edit50 = mapping5->add_edit();
                edit50->set_from_length(3);
                edit50->set_to_length(3);
                
                subpath3->set_score(6);
                
                identify_start_subpaths(multipath_aln);
                
                REQUIRE( genome.optimal_score_on_genome(multipath_aln, graph) == 7 - 4 + 6 );
                
            }
        }
    }
}
