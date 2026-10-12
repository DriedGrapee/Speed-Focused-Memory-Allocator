# gdb setup for mm_alloc. gdb skips a local .gdbinit unless the directory is in
# its auto-load safe-path, hence `gdb -x .gdbinit` (what `make gdb` runs).
# Pending breakpoints let it work with dlopen'd mm_test too.

set breakpoint pending on
set print pretty on
set history save on
set confirm off

# Decode block header, flags, and links/footer if free
define blk
  set $b = (free_list *)($arg0)
  set $sz = (unsigned long)($b->header & ~3UL)
  printf "%p  size=%-6lu alloc=%d prev_alloc=%d", $b, $sz, (int)($b->header & 1UL), (int)(($b->header >> 1) & 1UL)
  if ($b->header & 1UL) == 0
    printf "  footer=%-6lu prev=%p next=%p", *(unsigned long *)((unsigned char *)$b + $sz - sizeof(size_t)), $b->body.links.prev, $b->body.links.next
  end
  printf "\n"
end
document blk
Print block size, flags, and links/footer if free.
end

# Walk program_free_list (bounded at 32 nodes)
define flist
  printf "program_free_list = %p\n", program_free_list
  set $n = program_free_list
  set $i = 0
  while $n != 0 && $i < 32
    printf "[%2d] ", $i
    blk $n
    set $n = $n->body.links.next
    set $i = $i + 1
  end
  if $n != 0
    printf "stopped after 32 nodes -- list is probably cyclic\n"
  end
end
document flist
Walk program_free_list up to 32 nodes.
end

# Walk every block from $arg0 to sbrk(0), free or not: catches blocks missing from
# the free list and stale prev_alloc bits
define hwalk
  set $b = (free_list *)($arg0)
  set $end = (unsigned char *)sbrk(0)
  set $i = 0
  printf "heap ends at %p\n", $end
  while (unsigned char *)$b < $end && $i < 64
    set $s = (unsigned long)($b->header & ~3UL)
    if $s == 0
      printf "[%2d] %p  size=0 -- corrupt header, stopping\n", $i, $b
      loop_break
    end
    printf "[%2d] ", $i
    blk $b
    set $b = (free_list *)((unsigned char *)$b + $s)
    set $i = $i + 1
  end
end
document hwalk
Walk blocks linearly from ARG0 to sbrk(0).
end

# File-qualified: mm_test.c has globals with the same names
define bpalloc
  break mm_alloc.c:mm_malloc
  break mm_alloc.c:mm_free
  break mm_alloc.c:coalesce
  break mm_alloc.c:split
end
document bpalloc
Set breakpoints on mm_malloc, mm_free, coalesce, split.
end
