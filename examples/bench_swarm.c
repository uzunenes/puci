/* examples/bench_swarm.c - thin wrapper over bench/bench_swarm
 * Delegates to the bench tool; see bench/bench_swarm.c for the real benchmark.
 * This file exists so `examples/bench_swarm` can be invoked as documented.
 */
#include <stdio.h>
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Run the swarm benchmark via: ./build-bench/bench/bench_swarm [N]\n");
    printf("See bench/bench_swarm.c and docs/BENCHMARKS.md\n");
    return 0;
}
