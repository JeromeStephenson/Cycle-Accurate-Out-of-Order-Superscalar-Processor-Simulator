#include <iostream>
#include <vector>
#include <list>
#include <string>
#include <iomanip>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <sstream>
#include <memory>
#include <map>

// ============================================================================
// SECTION 1: GLOBAL SIMULATOR DEFINITIONS & TYPES
// ============================================================================
enum class OpType { ADD, SUB, MUL, DIV, LOAD, STORE, BRANCH, HALT };

const std::string OpNames[] = { "ADD", "SUB", "MUL", "DIV", "LOAD", "STORE", "BRANCH", "HALT" };

struct Instruction {
    uint64_t pc = 0;
    OpType op = OpType::HALT;
    int dest = -1; 
    int src1 = -1; 
    int src2 = -1; 
    int64_t immediate = 0;
    uint64_t mem_addr = 0;       
    bool branch_taken = false;       
    bool predicted_taken = false;
    uint64_t uid = 0; // Unique sequence identifier
};

// Logger helpers for cycle-by-cycle traces
class PipelineLogger {
public:
    static void log_stage(uint64_t cycle, const std::string& stage, const std::string& message) {
        std::cout << "[Cycle " << std::setw(4) << cycle << "] [" 
                  << std::setw(10) << stage << "] " << message << "\n";
    }
};

// ============================================================================
// SECTION 2: HARDWARE MEMORY SUBSYSTEM PERFORMANCE MODEL
// ============================================================================
struct CacheStats {
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t read_hits = 0;
    uint64_t read_misses = 0;
    uint64_t write_hits = 0;
    uint64_t write_misses = 0;
    uint64_t writebacks = 0;
};

struct CacheLine {
    uint64_t tag = 0;
    bool valid = false;
    bool dirty = false;
    uint32_t last_used_cycle = 0;
};

class HardwareCache {
private:
    uint64_t size;
    uint64_t associativity;
    uint64_t block_size;
    uint64_t num_sets;
    uint64_t index_mask;
    int index_shift;
    int tag_shift;
    std::vector<std::vector<CacheLine>> sets;

public:
    CacheStats stats;
    uint64_t hit_latency;
    std::string name;

    HardwareCache(const std::string& cache_name, uint64_t size_bytes, uint64_t assoc, uint64_t block_sz, uint64_t latency)
        : size(size_bytes), associativity(assoc), block_size(block_sz), hit_latency(latency), name(cache_name) {
        num_sets = size / (associativity * block_size);
        sets.resize(num_sets, std::vector<CacheLine>(associativity));
        index_shift = static_cast<int>(std::log2(block_size));
        tag_shift = index_shift + static_cast<int>(std::log2(num_sets));
        index_mask = num_sets - 1;
    }

    void parse_address(uint64_t address, uint64_t& index, uint64_t& tag) const {
        index = (address >> index_shift) & index_mask;
        tag = address >> tag_shift;
    }

    bool lookup_and_update(uint64_t address, bool is_write, uint64_t current_cycle, bool& evicted_dirty, uint64_t& evicted_addr) {
        uint64_t index, tag;
        parse_address(address, index, tag);
        auto& set = sets[index];
        evicted_dirty = false;

        if (is_write) stats.writes++; else stats.reads++;

        // Hit Detection
        for (uint64_t i = 0; i < associativity; ++i) {
            if (set[i].valid && set[i].tag == tag) {
                if (is_write) {
                    stats.write_hits++;
                    set[i].dirty = true;
                } else {
                    stats.read_hits++;
                }
                set[i].last_used_cycle = current_cycle;
                return true;
            }
        }

        // Miss path
        if (is_write) stats.write_misses++; else stats.read_misses++;
        return false;
    }

    void allocate_line(uint64_t address, bool is_dirty, uint64_t current_cycle, bool& evicted_dirty, uint64_t& evicted_address) {
        uint64_t index, tag;
        parse_address(address, index, tag);
        auto& set = sets[index];
        evicted_dirty = false;

        // Find invalid slot or evaluate LRU block for eviction
        int target_way = -1;
        uint32_t oldest_cycle = 0xFFFFFFFF;

        for (uint64_t i = 0; i < associativity; ++i) {
            if (!set[i].valid) {
                target_way = static_cast<int>(i);
                break;
            }
            if (set[i].last_used_cycle < oldest_cycle) {
                oldest_cycle = set[i].last_used_cycle;
                target_way = static_cast<int>(i);
            }
        }

        // Handle eviction processing rules
        if (set[target_way].valid) {
            if (set[target_way].dirty) {
                evicted_dirty = true;
                evicted_address = (set[target_way].tag << tag_shift) | (index << index_shift);
                stats.writebacks++;
            }
        }

        set[target_way].valid = true;
        set[target_way].tag = tag;
        set[target_way].dirty = is_dirty;
        set[target_way].last_used_cycle = current_cycle;
    }
};

class MemoryHierarchy {
public:
    HardwareCache l1;
    HardwareCache l2;
    uint64_t main_memory_latency;

    MemoryHierarchy(uint64_t main_mem_lat)
        : l1("L1_Cache", 32768, 8, 64, 2), 
          l2("L2_Cache", 262144, 16, 64, 12), 
          main_memory_latency(main_mem_lat) {}

    uint64_t access_latency_delay(uint64_t address, bool is_write, uint64_t current_cycle) {
        uint64_t cumulative_latency = l1.hit_latency;
        bool evicted_dirty = false;
        uint64_t evicted_addr = 0;

        bool l1_hit = l1.lookup_and_update(address, is_write, current_cycle, evicted_dirty, evicted_addr);
        if (l1_hit) {
            if (is_write) { // Write-Through pattern to L2
                cumulative_latency += l2.hit_latency;
                bool l2_hit = l2.lookup_and_update(address, true, current_cycle, evicted_dirty, evicted_addr);
                if (!l2_hit) {
                    cumulative_latency += main_memory_latency;
                    l2.allocate_line(address, true, current_cycle, evicted_dirty, evicted_addr);
                }
            }
            return cumulative_latency;
        }

        // L1 Miss Path -> Read L2 Block
        cumulative_latency += l2.hit_latency;
        bool l2_hit = l2.lookup_and_update(address, is_write, current_cycle, evicted_dirty, evicted_addr);

        if (l2_hit) {
            if (!is_write) {
                l1.allocate_line(address, false, current_cycle, evicted_dirty, evicted_addr);
            }
        } else {
            // Memory Fallback Layer
            cumulative_latency += main_memory_latency;
            if (!is_write) {
                l2.allocate_line(address, false, current_cycle, evicted_dirty, evicted_addr);
                l1.allocate_line(address, false, current_cycle, evicted_dirty, evicted_addr);
            } else {
                l2.allocate_line(address, true, current_cycle, evicted_dirty, evicted_addr);
            }
        }
        return cumulative_latency;
    }
};

// ============================================================================
// SECTION 3: ADVANCED GSHARE BRANCH PREDICTION SYSTEM
// ============================================================================
class GshareBranchPredictor {
private:
    uint16_t global_history_register;
    uint16_t history_mask;
    std::vector<uint8_t> pattern_history_table; // 2-bit saturating counters

public:
    uint64_t total_predictions = 0;
    uint64_t total_mispredictions = 0;

    GshareBranchPredictor(int history_bits) {
        global_history_register = 0;
        history_mask = (1 << history_bits) - 1;
        pattern_history_table.resize(1 << history_bits, 2); // Initialized to 2 (Weakly Taken)
    }

    bool predict(uint64_t pc) {
        total_predictions++;
        uint64_t pht_index = (pc ^ global_history_register) & history_mask;
        uint8_t counter_state = pattern_history_table[pht_index];
        return (counter_state >= 2);
    }

    void update_predictor(uint64_t pc, bool actual_outcome) {
        uint64_t pht_index = (pc ^ global_history_register) & history_mask;
        uint8_t counter_state = pattern_history_table[pht_index];

        bool predicted_state = (counter_state >= 2);
        if (predicted_state != actual_outcome) {
            total_mispredictions++;
        }

        if (actual_outcome) {
            if (pattern_history_table[pht_index] < 3) pattern_history_table[pht_index]++;
        } else {
            if (pattern_history_table[pht_index] > 0) pattern_history_table[pht_index]--;
        }

        // Shift in actual branch outcome to the global history register
        global_history_register = ((global_history_register << 1) | (actual_outcome ? 1 : 0)) & history_mask;
    }
};

// ============================================================================
// SECTION 4: LOAD-STORE QUEUE (LSQ) WITH MEMORY DISAMBIGUATION
// ============================================================================
struct LSQEntry {
    uint64_t uid = 0;
    int rob_id = -1;
    bool is_store = false;
    uint64_t address = 0;
    bool address_valid = false;
    int64_t store_data = 0;
    bool data_valid = false;
    bool executed = false;
};

class LoadStoreQueue {
private:
    size_t capacity;

public:
    std::vector<LSQEntry> queue;

    LoadStoreQueue(size_t max_capacity) : capacity(max_capacity) {}

    bool is_full() const { return queue.size() >= capacity; }
    void clear() { queue.clear(); }

    void insert_operation(uint64_t uid, int rob_id, bool is_store) {
        LSQEntry entry;
        entry.uid = uid;
        entry.rob_id = rob_id;
        entry.is_store = is_store;
        entry.address_valid = false;
        entry.data_valid = false;
        entry.executed = false;
        queue.push_back(entry);
    }

    void update_address(uint64_t uid, uint64_t computed_address) {
        for (auto& entry : queue) {
            if (entry.uid == uid) {
                entry.address = computed_address;
                entry.address_valid = true;
                break;
            }
        }
    }

    void update_store_payload(uint64_t uid, int64_t value) {
        for (auto& entry : queue) {
            if (entry.uid == uid) {
                entry.store_data = value;
                entry.data_valid = true;
                break;
            }
        }
    }

    // Evaluates memory forwarding mechanics
    bool check_store_forwarding(uint64_t uid, uint64_t load_addr, int64_t& forwarded_value) {
        int target_index = -1;
        for (size_t i = 0; i < queue.size(); ++i) {
            if (queue[i].uid == uid) {
                target_index = static_cast<int>(i);
                break;
            }
        }
        if (target_index <= 0) return false;

        // Search backward for the closest older store to the same memory address
        for (int i = target_index - 1; i >= 0; --i) {
            if (queue[i].is_store) {
                if (queue[i].address_valid && queue[i].address == load_addr) {
                    if (queue[i].data_valid) {
                        forwarded_value = queue[i].store_data;
                        return true; // Dynamic speculative forwarding match found
                    } else {
                        return false; // Age conflict: Address matches but payload isn't ready
                    }
                }
            }
        }
        return false;
    }

    void remove_retired_head(int rob_id) {
        if (!queue.empty() && queue.front().rob_id == rob_id) {
            queue.erase(queue.begin());
        }
    }
};

// ============================================================================
// SECTION 5: REGISTER RENAMING, RESERVATION STATIONS, AND ROB
// ============================================================================
struct RegisterRenameStatus {
    bool is_renamed = false;
    int rob_tag = -1;
};

struct ReservationStationSlot {
    bool busy = false;
    OpType op = OpType::HALT;
    int rob_id = -1;
    uint64_t inst_uid = 0;
    
    bool qj_ready = true; int qj_rob_tag = -1; int64_t vj_value = 0;
    bool qk_ready = true; int qk_rob_tag = -1; int64_t vk_value = 0;
    
    int64_t imm_operand = 0;
    uint64_t calculated_target_addr = 0;
};

struct ReorderBufferSlot {
    bool valid = false;
    bool ready = false;
    uint64_t pc = 0;
    OpType op = OpType::HALT;
    int target_destination_arf = -1;
    int64_t computed_result = 0;
    bool branch_taken_outcome = false;
    bool mispredicted = false;
    uint64_t instruction_uid = 0;
};

struct ActiveExecutionUnit {
    int rob_id = -1;
    int rs_index = -1;
    uint32_t cycles_until_completion = 0;
    int64_t calculated_output = 0;
    bool branch_outcome = false;
    uint64_t data_memory_addr = 0;
    OpType op_category = OpType::HALT;
};

// ============================================================================
// SECTION 6: THE SUPERSCALAR OUT-OF-ORDER SIMULATOR ARCHITECTURE
// ============================================================================
class SuperscalarProcessorCore {
private:
    size_t dispatch_width;
    size_t rob_capacity;
    size_t rs_capacity;

    std::vector<int64_t> ARF;
    std::vector<RegisterRenameStatus> RAT;
    std::vector<ReservationStationSlot> RS;
    std::vector<ReorderBufferSlot> ROB;
    std::list<ActiveExecutionUnit> execution_pipeline_slots;
    
    LoadStoreQueue lsq;
    GshareBranchPredictor branch_predictor;
    MemoryHierarchy memory_subsystem;

    // Architectural Pointer Index Tracking Counters
    size_t rob_head = 0;
    size_t rob_tail = 0;
    size_t current_rob_size = 0;
    size_t trace_program_counter = 0;
    uint64_t internal_cycle_counter = 0;
    uint64_t instruction_uid_sequence = 0;

    // Simulation Stats Tracking Counters
    uint64_t metric_retired_instructions = 0;
    uint64_t metric_rob_structural_stalls = 0;
    uint64_t metric_rs_structural_stalls = 0;
    uint64_t metric_lsq_structural_stalls = 0;
    double simulation_total_energy_pj = 0.0;

    // Structural Power Consumption Constants
    const double PJ_ROB_ACCESS      = 2.1;
    const double PJ_RS_ACCESS       = 2.8;
    const double PJ_RAT_RENAME      = 0.9;
    const double PJ_EXEC_ALU_CYCLE  = 4.5;
    const double PJ_EXEC_MUL_CYCLE  = 12.2;

public:
    SuperscalarProcessorCore(size_t width, size_t rs_size, size_t rob_size, uint64_t mem_lat)
        : dispatch_width(width), rob_capacity(rob_size), rs_capacity(rs_size),
          lsq(rs_size * 2), branch_predictor(10), memory_subsystem(mem_lat) {
        ARF.resize(32, 5); // Seed standard registers with a default base value of 5
        ARF[0] = 0;        // Hardwired zero register modeling rule
        RAT.resize(32);
        RS.resize(rs_capacity);
        ROB.resize(rob_capacity);
    }

    bool is_rob_full() const { return current_rob_size >= rob_capacity; }

    int find_free_reservation_station() {
        for (size_t i = 0; i < RS.size(); ++i) {
            if (!RS[i].busy) return static_cast<int>(i);
        }
        return -1;
    }

    void trigger_pipeline_flush_recovery() {
        PipelineLogger::log_stage(internal_cycle_counter, "RECOVERY", "Flushing pipeline structures due to mispredicted branch path...");
        execution_pipeline_slots.clear();
        lsq.clear();
        for (size_t i = 0; i < rs_capacity; ++i) RS[i].busy = false;
        for (size_t i = 0; i < 32; ++i) RAT[i] = { false, -1 };
        for (size_t i = 0; i < rob_capacity; ++i) ROB[i].valid = false;
        
        rob_head = 0;
        rob_tail = 0;
        current_rob_size = 0;
    }

    void step_cycle(std::vector<Instruction>& program_trace) {
        internal_cycle_counter++;
        
        // Stage 1: In-Order Commit / Retirement
        process_retirement_stage();

        // Stage 2: Out-of-Order Writeback / Common Data Bus Broadcast
        process_writeback_stage();

        // Stage 3: Superscalar Dispatch / Issue
        process_dispatch_stage(program_trace);

        // Stage 4: Out-of-Order Execution Issue
        process_execution_stage();
    }

    void process_retirement_stage() {
        size_t retired_slots_this_cycle = 0;
        while (current_rob_size > 0 && retired_slots_this_cycle < dispatch_width) {
            ReorderBufferSlot& head_slot = ROB[rob_head];
            if (head_slot.valid && head_slot.ready) {
                simulation_total_energy_pj += PJ_ROB_ACCESS;

                // Branch verification check
                if (head_slot.op == OpType::BRANCH && head_slot.mispredicted) {
                    branch_predictor.update_predictor(head_slot.pc, head_slot.branch_taken_outcome);
                    trigger_pipeline_flush_recovery();
                    trace_program_counter = program_trace_end_sentinel(head_slot.pc); 
                    break;
                } else if (head_slot.op == OpType::BRANCH) {
                    branch_predictor.update_predictor(head_slot.pc, head_slot.branch_taken_outcome);
                }

                // Handle LSQ removal logic
                if (head_slot.op == OpType::LOAD || head_slot.op == OpType::STORE) {
                    lsq.remove_retired_head(static_cast<int>(rob_head));
                }

                // Update Architectural Register File State
                if (head_slot.target_destination_arf != -1 && head_slot.target_destination_arf != 0) {
                    ARF[head_slot.target_destination_arf] = head_slot.computed_result;
                    
                    if (RAT[head_slot.target_destination_arf].is_renamed && 
                        RAT[head_slot.target_destination_arf].rob_tag == static_cast<int>(rob_head)) {
                        RAT[head_slot.target_destination_arf].is_renamed = false;
                    }
                }

                std::stringstream msg;
                msg << "Instruction UID [" << head_slot.instruction_uid << "] PC [0x" 
                    << std::hex << head_slot.pc << "] retired successfully.";
                PipelineLogger::log_stage(internal_cycle_counter, "COMMIT", msg.str());

                head_slot.valid = false;
                rob_head = (rob_head + 1) % rob_capacity;
                current_rob_size--;
                metric_retired_instructions++;
                retired_slots_this_cycle++;
            } else {
                break; // Head block isn't ready
            }
        }
    }

    void process_writeback_stage() {
        std::vector<ActiveExecutionUnit> units_completed_this_cycle;
        for (auto it = execution_pipeline_slots.begin(); it != execution_pipeline_slots.end();) {
            it->cycles_until_completion--;
            if (it->cycles_until_completion == 0) {
                units_completed_this_cycle.push_back(*it);
                it = execution_pipeline_slots.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& unit : units_completed_this_cycle) {
            std::stringstream msg;
            msg << "ROB ID [" << unit.rob_id << "] completed execution. Broadcasting result: " << unit.calculated_output;
            PipelineLogger::log_stage(internal_cycle_counter, "WRITEBACK", msg.str());

            ROB[unit.rob_id].ready = true;
            ROB[unit.rob_id].computed_result = unit.calculated_output;
            ROB[unit.rob_id].branch_taken_outcome = unit.branch_outcome;
            RS[unit.rs_index].busy = false;
            simulation_total_energy_pj += PJ_RS_ACCESS;

            // Handle LSQ updates for stores
            if (unit.op_category == OpType::STORE) {
                lsq.update_store_payload(ROB[unit.rob_id].instruction_uid, unit.calculated_output);
            }

            // Common Data Bus Forwarding/Wakeup Logic Loop
            for (auto& slot : RS) {
                if (slot.busy) {
                    if (!slot.qj_ready && slot.qj_rob_tag == unit.rob_id) {
                        slot.vj_value = unit.calculated_output;
                        slot.qj_ready = true;
                    }
                    if (!slot.qk_ready && slot.qk_rob_tag == unit.rob_id) {
                        slot.vk_value = unit.calculated_output;
                        slot.qk_ready = true;
                    }
                }
            }
        }
    }

    void process_dispatch_stage(std::vector<Instruction>& program_trace) {
        size_t instructions_dispatched_this_cycle = 0;

        while (trace_program_counter < program_trace.size() && instructions_dispatched_this_cycle < dispatch_width) {
            if (is_rob_full()) {
                metric_rob_structural_stalls++;
                PipelineLogger::log_stage(internal_cycle_counter, "DISPATCH", "Stall encountered: Reorder Buffer Full.");
                break;
            }

            int rs_slot = find_free_reservation_station();
            if (rs_slot == -1) {
                metric_rs_structural_stalls++;
                PipelineLogger::log_stage(internal_cycle_counter, "DISPATCH", "Stall encountered: Reservation Stations Full.");
                break;
            }

            Instruction& inst = program_trace[trace_program_counter];

            if ((inst.op == OpType::LOAD || inst.op == OpType::STORE) && lsq.is_full()) {
                metric_lsq_structural_stalls++;
                PipelineLogger::log_stage(internal_cycle_counter, "DISPATCH", "Stall encountered: Load-Store Queue Full.");
                break;
            }

            int assigned_rob_tag = static_cast<int>(rob_tail);
            instruction_uid_sequence++;
            inst.uid = instruction_uid_sequence;

            // Handle Branch Target Buffer prediction interface
            if (inst.op == OpType::BRANCH) {
                inst.predicted_taken = branch_predictor.predict(inst.pc);
                if (inst.predicted_taken != inst.branch_taken) {
                    ROB[assigned_rob_tag].mispredicted = true;
                }
            }

            // Structure data inside Load Store Queue
            if (inst.op == OpType::LOAD || inst.op == OpType::STORE) {
                lsq.insert_operation(inst.uid, assigned_rob_tag, (inst.op == OpType::STORE));
            }

            // Update Reorder Buffer Slot tracking payload
            ROB[assigned_rob_tag].valid = true;
            ROB[assigned_rob_tag].ready = false;
            ROB[assigned_rob_tag].pc = inst.pc;
            ROB[assigned_rob_tag].op = inst.op;
            ROB[assigned_rob_tag].target_destination_arf = inst.dest;
            ROB[assigned_rob_tag].instruction_uid = inst.uid;

            rob_tail = (rob_tail + 1) % rob_capacity;
            current_rob_size++;

            // Populate Reservation Station parameters and rename operands
            auto& rs = RS[rs_slot];
            rs.busy = true;
            rs.op = inst.op;
            rs.rob_id = assigned_rob_tag;
            rs.inst_uid = inst.uid;
            rs.imm_operand = inst.immediate;

            // Source Operand 1 Renaming
            if (inst.src1 != -1) {
                if (RAT[inst.src1].is_renamed) {
                    int producer_rob = RAT[inst.src1].rob_tag;
                    if (ROB[producer_rob].ready) {
                        rs.vj_value = ROB[producer_rob].computed_result;
                        rs.qj_ready = true;
                    } else {
                        rs.qj_rob_tag = producer_rob;
                        rs.qj_ready = false;
                    }
                } else {
                    rs.vj_value = ARF[inst.src1];
                    rs.qj_ready = true;
                }
                simulation_total_energy_pj += PJ_RAT_RENAME;
            } else {
                rs.qj_ready = true;
            }

            // Source Operand 2 Renaming
            if (inst.src2 != -1) {
                if (RAT[inst.src2].is_renamed) {
                    int producer_rob = RAT[inst.src2].rob_tag;
                    if (ROB[producer_rob].ready) {
                        rs.vk_value = ROB[producer_rob].computed_result;
                        rs.qk_ready = true;
                    } else {
                        rs.qk_rob_tag = producer_rob;
                        rs.qk_ready = false;
                    }
                } else {
                    rs.vk_value = ARF[inst.src2];
                    rs.qk_ready = true;
                }
                simulation_total_energy_pj += PJ_RAT_RENAME;
            } else {
                rs.qk_ready = true;
            }

            // Mark destination mapping inside the Register Alias Table (RAT)
            if (inst.dest != -1 && inst.dest != 0) {
                RAT[inst.dest].is_renamed = true;
                RAT[inst.dest].rob_tag = assigned_rob_tag;
            }

            std::stringstream msg;
            msg << "Dispatched instruction " << OpNames[static_cast<int>(inst.op)] 
                << " to ROB Tag [" << assigned_rob_tag << "], RS Index [" << rs_slot << "]";
            PipelineLogger::log_stage(internal_cycle_counter, "DISPATCH", msg.str());

            trace_program_counter++;
            instructions_dispatched_this_cycle++;
        }
    }

    void process_execution_stage() {
        for (size_t i = 0; i < RS.size(); ++i) {
            auto& slot = RS[i];
            if (slot.busy && slot.qj_ready && slot.qk_ready) {
                
                // Ensure structural execution lock to prevent redundant dispatches
                bool active_in_funit = false;
                for (const auto& unit : execution_pipeline_slots) {
                    if (unit.rob_id == slot.rob_id) { active_in_funit = true; break; }
                }
                if (active_in_funit) continue;

                ActiveExecutionUnit unit;
                unit.rob_id = slot.rob_id;
                unit.rs_index = static_cast<int>(i);
                unit.op_category = slot.op;
                unit.branch_outcome = false;

                // ALU Math execution processing routing tables
                if (slot.op == OpType::ADD) {
                    unit.calculated_output = slot.vj_value + (slot.qk_ready ? slot.vk_value : slot.imm_operand);
                    unit.cycles_until_completion = 1;
                    simulation_total_energy_pj += PJ_EXEC_ALU_CYCLE;
                } 
                else if (slot.op == OpType::SUB) {
                    unit.calculated_output = slot.vj_value - slot.vk_value;
                    unit.cycles_until_completion = 1;
                    simulation_total_energy_pj += PJ_EXEC_ALU_CYCLE;
                } 
                else if (slot.op == OpType::MUL) {
                    unit.calculated_output = slot.vj_value * slot.vk_value;
                    unit.cycles_until_completion = 3;
                    simulation_total_energy_pj += PJ_EXEC_MUL_CYCLE;
                } 
                else if (slot.op == OpType::BRANCH) {
                    unit.calculated_output = 0;
                    unit.cycles_until_completion = 1;
                    unit.branch_outcome = (slot.vj_value == slot.vk_value);
                    simulation_total_energy_pj += PJ_EXEC_ALU_CYCLE;
                } 
                else if (slot.op == OpType::LOAD) {
                    uint64_t target_mem_addr = slot.vj_value + slot.imm_operand;
                    slot.calculated_target_addr = target_mem_addr;
                    lsq.update_address(slot.inst_uid, target_mem_addr);

                    int64_t forwarded_val = 0;
                    if (lsq.check_store_forwarding(slot.inst_uid, target_mem_addr, forwarded_val)) {
                        unit.calculated_output = forwarded_val;
                        unit.cycles_until_completion = 1; // Accelerated store forwarding bypass latency hit
                        PipelineLogger::log_stage(internal_cycle_counter, "FORWARD", "Data forwarded from LSQ store entry.");
                    } else {
                        uint64_t mem_delay = memory_subsystem.access_latency_delay(target_mem_addr, false, internal_cycle_counter);
                        unit.calculated_output = 99; // Mock fetched data from cache architecture layout
                        unit.cycles_until_completion = static_cast<uint32_t>(mem_delay);
                    }
                } 
                else if (slot.op == OpType::STORE) {
                    uint64_t target_mem_addr = slot.vj_value + slot.imm_operand;
                    slot.calculated_target_addr = target_mem_addr;
                    lsq.update_address(slot.inst_uid, target_mem_addr);
                    
                    unit.calculated_output = slot.vk_value; // Store data value payload
                    uint64_t mem_delay = memory_subsystem.access_latency_delay(target_mem_addr, true, internal_cycle_counter);
                    unit.cycles_until_completion = static_cast<uint32_t>(mem_delay);
                }

                execution_pipeline_slots.push_back(unit);
                std::stringstream msg;
                msg << "Fired Execution Unit for ROB ID [" << slot.rob_id << "]. Expected delay: " << unit.cycles_until_completion;
                PipelineLogger::log_stage(internal_cycle_counter, "EXECUTE", msg.str());
            }
        }
    }

    size_t program_trace_end_sentinel(uint64_t failed_branch_pc) {
        // Mock trace redirection layout by resetting back to 0 or terminal end configuration
        return 0xFFFFFF0; // High sentinel bound forcing automated pipeline clear boundaries
    }

    void dump_system_performance_report() {
        std::cout << "\n";
        std::cout << "======================================================================\n";
        std::cout << "                  CYCLE-ACCURATE CORE PERFORMANCE REPORT             \n";
        std::cout << "======================================================================\n";
        std::cout << " Total Clock Runtime Cycles     : " << internal_cycle_counter << " cycles\n";
        std::cout << " Instructions Committed Successfully: " << metric_retired_instructions << "\n";
        std::cout << " Achieved Instructions-Per-Cycle: " << std::setprecision(3) << (double)metric_retired_instructions / internal_cycle_counter << " IPC\n";
        std::cout << " Gshare Branch Predictions Hit  : " << branch_predictor.total_predictions - branch_predictor.total_mispredictions << "\n";
        std::cout << " Gshare Branch Prediction Misses: " << branch_predictor.total_mispredictions << "\n";
        std::cout << " Structural ROB Stall Conditions: " << metric_rob_structural_stalls << " cycles\n";
        std::cout << " Structural RS Stall Conditions : " << metric_rs_structural_stalls << " cycles\n";
        std::cout << " Structural LSQ Stall Conditions: " << metric_lsq_structural_stalls << " cycles\n";
        std::cout << " Total Dynamic Energy Consumed  : " << simulation_total_energy_pj << " pJ\n";
        std::cout << "----------------------------------------------------------------------\n";
        std::cout << " L1 Cache Reads  : " << std::setw(6) << memory_subsystem.l1.stats.reads << " | L1 Cache Hits  : " << memory_subsystem.l1.stats.read_hits << "\n";
        std::cout << " L1 Cache Writes : " << std::setw(6) << memory_subsystem.l1.stats.writes << " | L1 Cache Misses: " << memory_subsystem.l1.stats.read_misses << "\n";
        std::cout << " L2 Cache Reads  : " << std::setw(6) << memory_subsystem.l2.stats.reads << " | L2 Cache Hits  : " << memory_subsystem.l2.stats.read_hits << "\n";
        std::cout << " L2 Cache Writes : " << std::setw(6) << memory_subsystem.l2.stats.writes << " | L2 Cache Misses: " << memory_subsystem.l2.stats.read_misses << "\n";
        std::cout << "======================================================================\n";
    }

   bool is_pipeline_active(size_t trace_size) const {
    return (trace_program_counter < trace_size) ||
           (current_rob_size > 0) ||
           (!execution_pipeline_slots.empty());
}
};

// ============================================================================
// SECTION 7: MAIN EXECUTION MATRIX DRIVER ENVIRONMENT
// ============================================================================
int main() {
    // Configure System Topology: 4-Wide Superscalar Issue, 8 Reservation Stations, 16 ROB slots, 40-cycle Memory Latency
    SuperscalarProcessorCore core(4, 8, 16, 40);

    // Mock an expanded assembly instruction sequence stream inside memory arrays
    std::vector<Instruction> programmatic_trace = {
        { 0x1004, OpType::ADD,    1,  2,  3, 0,  false }, // R1 = R2 + R3
        { 0x1008, OpType::MUL,    4,  1,  5, 0,  false }, // R4 = R1 * R5 (RAW Dependency on R1)
        { 0x100C, OpType::STORE, -1,  6,  4, 32, false }, // Mem[R6 + 32] = R4 (RAW Dependency on R4)
        { 0x1010, OpType::LOAD,   7,  6, -1, 32, false }, // R7 = Mem[R6 + 32] (Store Forwarding Target!)
        { 0x1014, OpType::SUB,    8,  7,  2, 0,  false }, // R8 = R7 - R2
        { 0x1018, OpType::BRANCH, -1, 8,  0, 0,  false }, // Branch IF R8 == R0 (Predicted Not Taken, Actual Not Taken)
        { 0x101C, OpType::ADD,    9,  1,  2, 0,  false }  // Fall-through computation instruction path
    };

    std::cout << "======================================================================\n";
    std::cout << "                INITIATING CYCLE-ACCURATE OOO CORE MATRIX              \n";
    std::cout << "======================================================================\n";

    // Standard simulation ticking loop
   while (core.is_pipeline_active(programmatic_trace.size())) {
    core.step_cycle(programmatic_trace);
}
    core.dump_system_performance_report();

    return 0;
}
