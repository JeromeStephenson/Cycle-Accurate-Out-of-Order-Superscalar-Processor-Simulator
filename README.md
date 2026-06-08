# Cycle-Accurate-Out-of-Order-Superscalar-Processor-Simulator
A detailed C++ microarchitecture simulator modeling modern OoO execution, branch prediction, cache hierarchy, and dynamic scheduling techniques used in contemporary CPUs.
# Project Description

Cycle-Accurate Superscalar Out-of-Order Processor Simulator

A C++ cycle-accurate simulator that models the behavior of a modern superscalar out-of-order processor. The simulator implements key microarchitectural structures including Register Renaming, Reservation Stations, Reorder Buffer (ROB), Load-Store Queue (LSQ), Gshare Branch Prediction, and a multi-level cache hierarchy. It supports out-of-order execution with in-order retirement, branch speculation and recovery, memory dependency handling, and detailed cycle-by-cycle performance tracing.

# Features
        4-wide superscalar dispatch architecture
        Register Alias Table (RAT) based register renaming
        Tomasulo-style dynamic scheduling using Reservation Stations
        Reorder Buffer (ROB) for precise exceptions and in-order commit
        Load-Store Queue (LSQ) with memory forwarding support
        Gshare branch predictor with speculative execution
        Two-level cache hierarchy (L1/L2) with configurable latencies
        Cycle-accurate execution modeling
        Performance statistics including IPC, cache behavior, branch prediction accuracy, and structural hazards
        Detailed execution logs for microarchitectural analysis
        Learning Objectives

This project demonstrates the fundamental concepts used in modern high-performance processors such as dynamic instruction scheduling, speculative execution, memory hierarchy modeling, and instruction-level parallelism (ILP).



## Implemented Components
Reorder Buffer (ROB)

Maintains program order and guarantees precise architectural state by retiring instructions in order.

Features:

In-order commit
Dependency tracking
Speculative state management
Branch recovery support
Register Renaming

Eliminates false dependencies through a Register Alias Table (RAT).

Benefits:

Removes WAR hazards
Removes WAW hazards
Improves instruction-level parallelism
Reservation Stations

Hold instructions waiting for operands.

Capabilities:

Dynamic scheduling
Operand wakeup
Dependency resolution
Out-of-order execution
Load Store Queue (LSQ)

Tracks memory operations and models memory disambiguation.

Features:

Load/store ordering
Address tracking
Store-to-load forwarding support
Branch Prediction

Implements a Gshare branch predictor.

Features:

Global history register
Pattern history table
2-bit saturating counters
Misprediction tracking
Cache Hierarchy

Models a two-level cache subsystem.

L1 Cache
Parameter	Value
Size	32 KB
Associativity	8-way
Block Size	64 B
Latency	2 cycles
L2 Cache
Parameter	Value
Size	256 KB
Associativity	16-way
Block Size	64 B
Latency	12 cycles
Main Memory
Parameter	Value
Latency	40 cycles
Performance Metrics

The simulator reports:

Total cycles
Instructions retired
Instructions per cycle (IPC)
Branch prediction accuracy
ROB stalls
Reservation Station stalls
LSQ stalls
Cache hits and misses
Dynamic energy estimation


This simulator demonstrates:

Instruction-Level Parallelism (ILP)
Tomasulo Scheduling
Register Renaming
Dynamic Scheduling
Out-of-Order Execution
Speculative Execution
Branch Prediction
Memory Hierarchy Design
Cache Modeling
Pipeline Recovery Mechanisms
Future Improvements


Recommended topics for understanding the simulator:

Tomasulo's Algorithm
Reorder Buffers
Register Renaming
Branch Prediction
Cache Architecture
Superscalar Processors
Out-of-Order Execution
Computer Architecture
License

This project is released under the MIT License.

Author

Jerome Stephenson

M.Tech Student | Computer Architecture Enthusiast

