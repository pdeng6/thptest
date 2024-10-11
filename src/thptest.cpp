#include <iostream>
#include <chrono>
#include <error.h>
#include <string.h>

#include <sys/mman.h>
#include <unistd.h>
#include <linux/kernel-page-flags.h> // KPF_THP

#include "CLI11.hpp"
#include "page-info.h"

// assume 2M large page
constexpr size_t PAGE_SIZE_2M = 2*1024*1024;
constexpr size_t PAGE_SIZE_4K = 4*1024;

int main(int argc, char* argv[]) {
  CLI::App app{"THP memory allocation and access test."};

  size_t MEM_SIZE = 4743168;
  app.add_option("-s,--memory-region-size", MEM_SIZE, "The size of a memory region need to be mmaped.");
  
  size_t MEM_COUNTS = 174;
  app.add_option("-c,--memory-region-counts", MEM_COUNTS, "The number of memory regions need to be created.");
  
  size_t ITER_COUNTS = 4;
  app.add_option("-i,--test-iterations", ITER_COUNTS, "The number of iterations for the performance test.");

  bool HUGE_PAGE = false;
  app.add_option("-l,--huage-page", HUGE_PAGE, "Whether or not place a madvise call.");
  
  CLI11_PARSE(app, argc, argv);
  
  void * mem_regions[MEM_COUNTS];
  
  for (size_t i =0; i < MEM_COUNTS; i++) {
    void* region = mmap(NULL, MEM_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    if (region == MAP_FAILED) {
      printf("mmap failed: %s\n", strerror(errno));
      return 0;
    }


    if (HUGE_PAGE) {
      if (madvise(region, MEM_SIZE, MADV_HUGEPAGE)){
        printf("madvise failed: %s\n", strerror(errno));
        return 0;
      }
    }
    mem_regions[i] = region;
  }

  
  size_t total_num_2m_pages = 0; // large page counts
  size_t total_num_4k_pages = 0;
  size_t total_num_requested_bytes =0;
  
  for (size_t i =0; i < MEM_COUNTS; i++) {
    void* region = mem_regions[i];
    for (size_t j=0; j < MEM_SIZE; j++) {
      static_cast<char*>(region)[j] = 0; // touch, page fault. put it here for alloc vma in a batch, then touch.
      total_num_requested_bytes++;
    }
  }
  
  for (size_t i =0; i < MEM_COUNTS; i++) {
    void* region = mem_regions[i];

    // detect hugepage
    page_info_array pinfo = get_info_for_range(region, (char*)region + MEM_SIZE);
    flag_count thp_count = get_flag_count(pinfo, KPF_THP);

    fprintf(stderr,"[%08d %p] ", (int)i, region);
    if (thp_count.pages_available) {
      total_num_2m_pages += thp_count.pages_set / (PAGE_SIZE_2M / PAGE_SIZE_4K);
      total_num_4k_pages += thp_count.pages_total - thp_count.pages_set;
      
      fprintf(stderr, "Source pages allocated with transparent hugepages: %4.1f%% (%lu total pages, %4.1f%% flagged)\n",
             100.0 * thp_count.pages_set / thp_count.pages_total,
             thp_count.pages_total,
             100.0 * thp_count.pages_available / thp_count.pages_total);
    } else {
      fprintf(stderr, "Couldn't determine hugepage info (you are probably not running as root)\n");
    }
    
    for (size_t j=0; j < pinfo.num_pages; j++) {
      page_info pi = pinfo.info[j];

      fprintf(stderr,"\t"); fprint_info_header(stderr);
      fprintf(stderr,"\t"); fprint_info_row(stderr, pi);
    }
    
    
    free_info_array(pinfo);
  }


  fprintf(stderr, "================================= summary =======================================\n");
  size_t total_num_bytes_4k_pages = total_num_4k_pages * PAGE_SIZE_4K;
  size_t total_num_bytes_2m_pages = total_num_2m_pages * PAGE_SIZE_2M;
  size_t total_num_allocated_bytes = total_num_bytes_4k_pages + total_num_bytes_2m_pages;
  fprintf(stderr, "total requested bytes, total allocated bytes, total 2m pages," \
          "total bytes of 2m pages, total 4k pages, total bytes of 4k pages\n");
  fprintf(stderr, "%zu,%zu,%zu,%zu,%zu,%zu\n", total_num_requested_bytes, total_num_allocated_bytes,
          total_num_2m_pages, total_num_bytes_2m_pages, total_num_4k_pages, total_num_bytes_4k_pages);
  fprintf(stderr, "=================================================================================\n");
  
  // benchmarking
  std::cout << "Warmup ... ";
  size_t sum = 0;
  for (size_t i =0; i < 2; i++) {
    for (size_t j =0; j < MEM_SIZE; j++) {
      for (size_t k=0; k < MEM_COUNTS; k++) {
        sum += static_cast<char*>(mem_regions[k])[j];
      }
    }
  }
  std::cout << "Done. " << std::endl;

  {
    std::cout << "Benchmarking stride access ... "; 
    const auto begin = std::chrono::high_resolution_clock::now();
    for (size_t i =0; i < ITER_COUNTS; i++) {
      for (size_t j =0; j < MEM_SIZE; j++) {
        for (size_t k=0; k < MEM_COUNTS; k++) {
          sum += static_cast<char*>(mem_regions[k])[j];
        }
      }
    }
    const auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Done. " << std::endl;
  
  
    std::chrono::nanoseconds duration = end - begin;
    std::chrono::nanoseconds::rep total_ns = duration.count();
    std::cout << "Benchmark result in seconds: " << (total_ns / 1000000000)
              << "." << (total_ns % 1000000000) << std::endl;
  }

  {
    std::cout << "Benchmarking sequential access ... "; 
    const auto begin = std::chrono::high_resolution_clock::now();
    for (size_t i =0; i < ITER_COUNTS; i++) {
      for (size_t j =0; j < MEM_COUNTS; j++) {
        for (size_t k=0; k < MEM_SIZE; k++) {
          sum += static_cast<char*>(mem_regions[j])[k];
        }
      }
    }
    const auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Done. " << std::endl;
  
  
    std::chrono::nanoseconds duration = end - begin;
    std::chrono::nanoseconds::rep total_ns = duration.count();
    std::cout << "Benchmark result in seconds: " << (total_ns / 1000000000)
              << "." << (total_ns % 1000000000) << std::endl;
  }


  fprintf(stderr, "================================= VMAs =======================================\n");
  auto pid = getpid();
  char cmd[256];
  snprintf(cmd, 256, "cat /proc/%d/maps >> /proc/%d/fd/2", pid, pid);
  system(cmd);
  fprintf(stderr, "================================= VMAs =======================================\n");
  
  for (size_t i =0; i < MEM_COUNTS; i++) {
    munmap(mem_regions[i], MEM_SIZE);
  }

  return sum;
}
