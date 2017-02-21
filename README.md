# FFQ: A Fast Single-Producer/Multiple-Consumer Concurrent FIFO Queue

This is an implementation of FFQ presented in the IPDPS 2017 paper (FFQ: A Fast Single-Producer/Multiple-Consumer Concurrent FIFO Queue).

Please note that the MPMC version of this queue depends on double-width compare-and-swap instruction.

The evaluation in paper used [wfqueue's][https://github.com/chaoran/fast-wait-free-queue] test framework (by Chaoran Yang, John Mellor-Crummey).
