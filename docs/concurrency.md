# Concurrency model

The language targets developers working with LLMs and web servers. Strong, resilient concurrency is a must.

**Pillars:**
- No shared, mutable memory.
- Data is shared through communication..
- Memory and crashes are isolated.

## Model
- Reference counting + Copy on write.
  - When an Actor uses shared immutable data, it effectively just uses a reference. No copy, very performant.
  - Only when data is mutated is the data copied into the Actor’s arena.
- Actor model.
  - A struct containing information about the actor.
  - A work stealing executor that runs actors on real hardware threads.
  - Each Actor has its own arena of memory and the runtime catches panics/exceptions and deletes that arena and restarts it. The memory is just deallocated, which is relatively cheap.
  - Allocating in an Arena is just a “pointer bump.”
  - For very large data structures, consider B-Tree or HAMT (Hash Array Mapped Trie) based collections in the stdlib. These allow "functional updates" where only the changed path is copied, keeping the overhead O(log n) rather than O(n).
- Local-only mutations with escape analysis
  - Local-only variables stay on the stack.
  - The compiler allows for mutation of variables within the stack.
  - Any variables that escape their local context are either copied or moved to the heap.
- Channels
  - Transfer ownership of data from one arena to another.

## Circular references
How to detect circular references with reference counting? A -> B -> A?

- Within an arena, they don’t matter. When Arena is cleaned  up (they should be short lived), they get dereferenced automatically when the arana is deallocated.
- Use the type graph to detect the circular references.
- The compiler could potentially detect weak references in a few ways:
  - If A points to B (strong), and then B points to A, it could automatically apply the weak pointer.
  - If A points to B with a next field, and B points to A with a prev field, the compiler would detect the weak reference by name. Identifiers with the name “parent”, “prev”, or “back” could always be enforced with weak references.
  - Only standard library structures can use circular references, providing structures for Lists, Deqeues, etc. Circular references are otherwise considered an error.
- Use a keyword or operator to flag a weak reference: `Node @prev` or `weak Node prev`.

## Bad Actors
- Resource quotas
  - For memory, each time the runtime asks for more memory for an Actor, it checks if it will exceed the max_area_memory_limit. If it does, the actor is killed. 
  - For CPU, each time a loop is performed, the runtime increments a counter until it releases control. If the counter exceeds the max_reduction_count_limit, the runtime assumes the thread is in an infinite loop and kills it. The Actor can “yield” by sending data through a channel.
  - I/O should reset the reduction_count to make sure it doesn't get cleaned up.
  - It may be worthwhile to not immediately kill arenas so that the supervisor can log or inspect the reason why the arena was killed.
- Heartbeats
  - As the Actor is working, it needs to update when it last “cycled.”
  - The supervisor checks each actor’s last cycle. If it hasn’t changed after a specific period of time, it concludes the Actor is hung or deadlocked and reapers it.
- Tuning the quotas. 
  - Should NOT be runtime configurable, only compile time.
  - There’s an argument that the user should be able to tune each call to spawn. Maybe in a future version but not immediately.
