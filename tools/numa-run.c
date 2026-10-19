#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/mempolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Keep the guest launcher independent of libc/libnuma versions in app images.
// Build it statically and exec the original image command without a shell.
#define MAX_RESOURCE_BITS (1024U * 1024U)
#define WORD_BITS (sizeof(unsigned long) * CHAR_BIT)

static int parse_number(const char **cursor, unsigned *result) {
  const char *start = *cursor;
  unsigned value = 0;
  if (*start < '0' || *start > '9')
    return 0;
  while (**cursor >= '0' && **cursor <= '9') {
    unsigned digit = (unsigned)(**cursor - '0');
    if (value > (MAX_RESOURCE_BITS - 1U - digit) / 10U)
      return 0;
    value = value * 10U + digit;
    ++*cursor;
  }
  *result = value;
  return 1;
}

static int parse_cpus(const char *text, unsigned long *mask) {
  const char *cursor = text;
  if (!*cursor)
    return 0;
  while (*cursor) {
    unsigned first, last;
    if (!parse_number(&cursor, &first))
      return 0;
    last = first;
    if (*cursor == '-') {
      ++cursor;
      if (!parse_number(&cursor, &last) || last < first)
        return 0;
    }
    for (unsigned cpu = first; cpu <= last; ++cpu)
      mask[cpu / WORD_BITS] |= 1UL << (cpu % WORD_BITS);
    if (*cursor == '\0')
      return 1;
    if (*cursor++ != ',' || !*cursor)
      return 0;
  }
  return 0;
}

static unsigned long *get_affinity(size_t *bytes) {
  for (*bytes = 128; *bytes <= MAX_RESOURCE_BITS / CHAR_BIT; *bytes *= 2) {
    unsigned long *mask = calloc(1, *bytes);
    if (!mask)
      return NULL;
    if (syscall(SYS_sched_getaffinity, 0, *bytes, mask) >= 0)
      return mask;
    int failure = errno;
    free(mask);
    if (failure != EINVAL) {
      errno = failure;
      return NULL;
    }
  }
  errno = EOVERFLOW;
  return NULL;
}

static void policy_failure(const char *operation) {
  fprintf(stderr, "[NUMA] Guest %s failed: %s; locality policy was not fully applied\n", operation, strerror(errno));
}

int main(int argc, char **argv) {
  if (argc < 5 || strcmp(argv[3], "--") != 0 || !argv[4][0]) {
    fprintf(stderr, "Usage: wolf-numa-run <node> <local-cpulist> -- <command> [args...]\n");
    return 125;
  }
  const char *node_text = argv[1];
  unsigned node;
  if (!parse_number(&node_text, &node) || *node_text) {
    fprintf(stderr, "[NUMA] Invalid guest NUMA node: %s\n", argv[1]);
    return 125;
  }
  const size_t local_bytes = MAX_RESOURCE_BITS / CHAR_BIT;
  unsigned long *local = calloc(1, local_bytes);
  if (!local) {
    fprintf(stderr, "[NUMA] Cannot allocate guest CPU mask: %s\n", strerror(errno));
    return 125;
  }
  if (!parse_cpus(argv[2], local)) {
    fprintf(stderr, "[NUMA] Invalid guest local CPU list: %s\n", argv[2]);
    free(local);
    return 125;
  }

  size_t allowed_bytes = 0;
  unsigned long *allowed = get_affinity(&allowed_bytes);
  int cpu_applied = 0;
  if (!allowed) {
    policy_failure("sched_getaffinity");
  } else {
    int available = 0;
    for (size_t index = 0; index < local_bytes / sizeof(unsigned long); ++index) {
      local[index] &= index < allowed_bytes / sizeof(unsigned long) ? allowed[index] : 0;
      available |= local[index] != 0;
    }
    if (!available) {
      fprintf(stderr, "[NUMA] Guest has no allowed CPUs local to node %u\n", node);
    } else if (syscall(SYS_sched_setaffinity, 0, local_bytes, local) == 0) {
      cpu_applied = 1;
    } else {
      policy_failure("sched_setaffinity");
    }
  }
  free(local);
  free(allowed);
  if (!cpu_applied) {
    fprintf(stderr, "[NUMA] Guest local CPU placement could not be enforced; refusing to execute the command\n");
    return 125;
  }

  const size_t memory_words = node / WORD_BITS + 1;
  unsigned long *memory = calloc(memory_words, sizeof(unsigned long));
  if (!memory) {
    fprintf(stderr, "[NUMA] Cannot allocate guest memory mask: %s\n", strerror(errno));
    return 125;
  }
  memory[node / WORD_BITS] |= 1UL << (node % WORD_BITS);
  int memory_applied = 0;
  if (syscall(SYS_set_mempolicy, MPOL_PREFERRED | MPOL_F_STATIC_NODES, memory, memory_words * WORD_BITS + 1) == 0) {
    memory_applied = 1;
  } else {
    policy_failure("set_mempolicy");
  }
  free(memory);
  fprintf(stderr,
          "[NUMA] Guest node %u: CPUs bound locally, memory %s\n",
          node,
          memory_applied ? "preferred locally with fallback" : "preference unavailable");

  execvp(argv[4], &argv[4]);
  const int failure = errno;
  fprintf(stderr, "[NUMA] Cannot execute guest command %s: %s\n", argv[4], strerror(failure));
  return failure == ENOENT ? 127 : 126;
}
