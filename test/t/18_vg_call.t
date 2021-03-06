#!/usr/bin/env bash

BASH_TAP_ROOT=../deps/bash-tap
. ../deps/bash-tap/bash-tap-bootstrap

PATH=../bin:$PATH # for vg


plan tests 3

# Toy example of hand-made pileup (and hand inspected truth) to make sure some
# obvious (and only obvious) SNPs are detected by vg call
vg view -J -v call/tiny.json > tiny.vg

vg view -J -l call/pileup.json -L > tiny.vgpu
vg call tiny.vg tiny.vgpu -A calls_l.vg -s 10 -d 10 -q 10 -b 0.25  > /dev/null 2> /dev/null
is $? "0" "vg call doesn't crash"

rm -f calls_l.json calls_l.vg

vg view -J -l call/pileup.json -L | vg call tiny.vg - -A calls_l.vg -s 10 -d 10 -q 10 -b 0.25 -f 0.2 > /dev/null 2> /dev/null
vg view -j calls_l.vg | jq . > calls_l.json
is $(jq --argfile a calls_l.json --argfile b call/truth_l.json -n '($a == $b)') true "vg call -l produces the expected output for toy snp test case."
rm -f calls_l.json calls_l.vg tiny.vg tiny.vgpu

echo '{"node": [{"id": 1, "sequence": "CGTAGCGTGGTCGCATAAGTACAGTAGATCCTCCCCGCGCATCCTATTTATTAAGTTAAT"}]}' | vg view -Jv - > test.vg
vg index -x test.xg -g test.gcsa -k 16 test.vg
true >reads.txt
for REP in seq 1 5; do
    echo 'CGTAGCGTGGTCGCATAAGTACAGTANATCCTCCCCGCGCATCCTATTTATTAAGTTAAT' >>reads.txt
done
vg map -x test.xg -g test.gcsa --reads reads.txt > test.gam
vg pileup test.vg test.gam > test.vgpu
vg call test.vg test.vgpu >/dev/null --aug-graph aug.vg

N_COUNT=$(vg view -j aug.vg | grep "N" | wc -l)

is "${N_COUNT}" "0" "N bases are not augmented into the graph"

rm -r reads.txt test.vg test.xg test.gcsa test.gcsa.lcp test.gam test.vgpu aug.vg




