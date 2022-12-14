#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <vector>

// Cache Simulator
class CacheSim {
public:
  // Constructor
  CacheSim(std::string input, unsigned bs, unsigned a, unsigned c, unsigned mp,
           unsigned wbp) {
    // Initialize of input file stream object
    infile.open(input);

    // Set all of our cache settings
    block_size = bs;
    associativity = a;
    capacity = c;
    miss_penalty = mp;
    dirty_wb_penalty = wbp;

    // Calculate the number of blocks
    // Assume this divides evenly
    auto num_blocks = capacity / block_size;

    // Create our cache based on the number of blocks
    tags.resize(num_blocks);
    dirty.resize(num_blocks);
    valid.resize(num_blocks);
    priority.resize(num_blocks);

    // Calculate values for traversal
    // Cache lines come in the following format:
    // |****** TAG ******|**** SET ****|** OFFSET **|
    // Calculate the number of offset bits
    auto block_bits = std::__popcount(block_size - 1);

    // Calculate the number of set bits, and create a mask of 1s
    set_offset = block_bits;
    auto sets = capacity / (block_size * associativity);
    set_mask = sets - 1;
    auto set_bits = std::__popcount(set_mask);

    // Calculate the bit-offset for the tag and create a mask of 1s
    // Always use 64-bit addresses
    tag_offset = block_bits + set_bits;
  }

  // Run the Simulation
  void run() {
    // Keep reading data from a file
    std::string line;
    while (std::getline(infile, line)) {
      // Get the data for the access
      auto [type, address, instructions] = parse_line(line);

      // Probe the cache
      auto [hit, dirty_wb] = probe(type, address);

      // Update the cache statistics
      update_stats(instructions, type, hit, dirty_wb);
    }
  }

  // Destructor
  ~CacheSim() {
    infile.close();
    dump_stats();
  }

private:
  // Input trace file
  std::ifstream infile;

  // Cache Settings
  unsigned block_size;
  unsigned associativity;
  unsigned capacity;
  unsigned miss_penalty;
  unsigned dirty_wb_penalty;

  // Access settings
  unsigned set_offset;
  unsigned tag_offset;
  unsigned set_mask;

  // The actual cache state
  std::vector<std::uint64_t> tags;
  std::vector<char> dirty;
  std::vector<char> valid;
  std::vector<int> priority;

  // Cache statistics
  std::int64_t writes_ = 0;
  std::int64_t mem_accesses_ = 0;
  std::int64_t misses_ = 0;
  std::int64_t dirty_wb_ = 0;
  std::int64_t instructions_ = 0;

  // Dump the statistics from simulation
  void dump_stats() {
    // Print the cache settings
    std::cout << "CACHE SETTINGS\n";
    std::cout << "       Cache Size (Bytes): " << capacity << '\n';
    std::cout << "           Associativity : " << associativity << '\n';
    std::cout << "       Block Size (Bytes): " << block_size << '\n';
    std::cout << "    Miss Penalty (Cycles): " << miss_penalty << '\n';
    std::cout << "Dirty WB Penalty (Cycles): " << dirty_wb_penalty << '\n';
    std::cout << '\n';

    // Print the access breakdown
    std::cout << "CACHE ACCESS STATS\n";
    std::cout << "TOTAL ACCESSES: " << mem_accesses_ << '\n';
    std::cout << "         READS: " << mem_accesses_ - writes_ << '\n';
    std::cout << "        WRITES: " << writes_ << '\n';
    std::cout << '\n';

    // Print the miss-rate breakdown
    std::cout << "CACHE MISS-RATE STATS\n";
    double miss_rate = (double)misses_ / (double)mem_accesses_ * 100.0;
    auto hits = mem_accesses_ - misses_;
    std::cout << "     MISS-RATE: " << miss_rate << '\n';
    std::cout << "        MISSES: " << misses_ << '\n';
    std::cout << "          HITS: " << hits << '\n';
    std::cout << '\n';

    // Print the instruction breakdown
    std::cout << "CACHE IPC STATS\n";
    auto cycles = miss_penalty * misses_;
    cycles += dirty_wb_penalty * dirty_wb_;
    cycles += instructions_;
    double ipc = (double)instructions_ / (double)cycles;
    std::cout << "           IPC: " << ipc << '\n';
    std::cout << "  INSTRUCTIONS: " << instructions_ << '\n';
    std::cout << "        CYCLES: " << cycles << '\n';
    std::cout << "      DIRTY WB: " << dirty_wb_ << '\n';
  }

  // Get memory access from the trace file
  std::tuple<bool, std::uint64_t, int> parse_line(std::string access) {
    // What we want to parse
    int type;
    std::uint64_t address;
    int instructions;

    // Parse the string we read from the file
    sscanf(access.c_str(), "# %d %llx %d", &type, &address, &instructions);

    return {type, address, instructions};
  }

  // Probe the cache
  std::tuple<bool, bool> probe(bool type, std::uint64_t address) {
    // Calculate the set from the address
    auto set = get_set(address);
    auto tag = get_tag(address);

    // Create a span for our set
    auto base = set * associativity;
    std::span local_tags{tags.data() + base, associativity};
    std::span local_dirty{dirty.data() + base, associativity};
    std::span local_valid{valid.data() + base, associativity};
    std::span local_priority{priority.data() + base, associativity};

    // Check each cache line in the set
    auto hit = false;
    int invalid_index = -1;
    int index;
    for (auto i = 0u; i < local_valid.size(); i++) {
      // Check if the block is invalid
      if (!local_valid[i]) {
        // Keep track of invalid entries in case we need them
        invalid_index = i;
        continue;
      }

      // Check if the tag matches
      if (tag != local_tags[i])
        continue;

      // We found the line so mark it is a hit
      hit = true;
      index = i;

      // Update the dirty flag
      local_dirty[index] |= type;

      // Break out of the loop
      break;
    }

    // Find an element to replace if it wasnt a hit
    auto dirty_wb = false;
    if (!hit) {
      // First try to use an invalid line (if available)
      if (invalid_index >= 0) {
        index = invalid_index;
        local_valid[index] = 1;
      }
      // Otherwise, evict the lowest-priority cache block (largest value)
      else {
        auto max_element = std::ranges : max_element(local_priority);
        index = std::distance(begin(local_priority), max_element);
        dirty_wb = local_dirty[index];
      }

      local_tags[index] = tag;
      local_dirty[index] = type;
    }

    // Update the priority
    // Go through each element
    // Increase the priority of all the blocks with a lower priority than the
    // one we are accessing
    // High priority -> Low priority = 0 -> associativity - 1
    std::transform(begin(local_priority), end(local_priority),
                   begin(local_priority), [&](int p) {
                     if (p <= local_priority[index] && p < associativity) {
                       return p + 1;
                     } else {
                       return p;
                     }
                   });
    // Currently accessed block has the highest priority [0]
    local_priority[index] = 0;

    return {hit, dirty_wb};
  }

  // Extract the set number
  // Shift the set to the bottom then extract the set set_bits
  int get_set(std::uint64_t address) {
    auto shifted_address = address >> set_offset;
    return shifted_address & set_mask;
  }

  // Extract the tag
  // Shift the tag to the bottom
  // No need to use mask (tag is all upper remaining bits)
  std::uint64_t get_tag(std::uint64_t address) { return address >> tag_offset; }

  // Update the stats
  void update_stats(int instructions, bool type, bool hit, bool dirty_wb) {
    mem_accesses_++;
    writes_ += type;
    misses_ += !hit;
    instructions_ += instructions;
    dirty_wb_ = dirty_wb;
  }

  int main(int argc, char *argv[]) {
    // Kill the program if we didn't get an input file
    assert(argc == 2);

    // File Location
    std::string location(argv[1]);

    // Hard coded cache settings
    unsigned block_size = 1 << 4;
    unsigned associativity = 1 << 0;
    unsigned capacity = 1 << 14;
    unsigned miss_penalty = 30;
    unsigned dirty_wb_penalty = 2;

    // Create our Simulator
    CacheSim simulator(location, block_size, associativity, capacity,
                       miss_penalty, dirty_wb_penalty);
    simulator.run();

    return 0;
  }
};
