# gdb setup for mm_alloc.
#
# Works with both binaries:
#   gdb ./mm_debug   direct-linked, symbols available immediately
#   gdb ./mm_test    dlopen'd, symbols bind when hw3lib.so loads (hence pending breakpoints)
#
# gdb refuses local .gdbinit files unless the directory is trusted. Either add
#   add-auto-load-safe-path /home/nicre/documents/programming/CS162_25fall_hws_starter_code/hw-memory/mm_alloc
# to ~/.config/gdb/gdbinit, or skip the config and run: gdb -x .gdbinit ./mm_debug

set breakpoint pending on
set print pretty on
set history save on
set confirm off

# Decode one block header. Free blocks also print their footer and list links.
# Usage: blk free_block   /   blk 0x5555555592a0
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
Print size, allocated bit, prev-allocated bit of a block; footer and links if free.
end

# Walk the free list from the head. Bounded, because a corrupted list is often cyclic.
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
Walk program_free_list and print each node. Bounded at 32 nodes.
end

# Walk the heap linearly from a block address to the current break.
# Every block is visited whether allocated or free, so this is what catches
# blocks missing from the free list, and prev_alloc flags out of sync.
# Usage: hwalk chosen_block
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
Walk blocks linearly from ARG0 to sbrk(0). Requires a running process.
end

# The allocator entry points. File-qualified because mm_test.c has globals
# of the same name, which would otherwise make the linespec ambiguous.
define bpalloc
  break mm_alloc.c:mm_malloc
  break mm_alloc.c:mm_free
  break mm_alloc.c:coalesce
  break mm_alloc.c:split
end
document bpalloc
Set breakpoints on mm_malloc, mm_free, coalesce and split.
end
