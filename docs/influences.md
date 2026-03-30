# Influences
Saga is the Norse goddess of History and Knowledge. She lives in Sökkvabekkr with waves crashing overhead. 
Odin visits her daily to drink from golden cups and either just listen and reflect, or to have deep discussions.

As the Goddess of knowledge, she seems an ideal patron of a programming language. Just as Saga and Odin engage in
deep discussion, a programmer engages with the code, compiler and runtime.

Many different languages have helped inspire the creation of this language. Rigorous coding asthetics and
resiliency fueled strong opinions of the language's desire. 

## Guiding principles
These are the guiding principles of Saga:

1. Simple elegance. The prime directive.
2. Promote clean, readable, maintainable code. Source code should be easy to read and reason about.
3. The syntax should feel familiar and comfortable, not alien.
4. There should be only one conanocal way to do something but expressive enough to allow the language to flow. Keywords and operators should be multipurpose but predictable. Their behaviours should be the logical extension of their core purposes.
5. Less is more, except when it’s at the cost of clarity. 
6. Favor explicit intent over implicit behaviour.

More generally, the language must be orthogonal, each part of the language should flow into the next. A user should 
never have to ask the question, "Why do I do X one way but Y another?"

## Languages
Saga is a love-letter to the languages I've enjoyed the most. Go is the most obvious inspiration, lending a
great deal to the syntax. Ruby has also been a very clear influence. Erlang's BEAM architecture inspired the
memory model and runtime.

Experiences from many other languages informed various decisions—sometimes as a model to follow, and other times as a cautionary tale of inconsistency and technical debt. These include C, C++, Python, and PHP.

In particular:

**Go:** For the "Type-after-Identifier" syntax, fast compilation, and the "Boring is Better" philosophy for core logic. Saga adopts Go’s structural subtyping (interfaces) and its focus on a small, powerful keyword set to ensure that the language remains easy to learn and hard to misuse.

**Ruby:** For the expressive syntax (like Saga's or blocks and loop accumulators) and a relentless focus on developer happiness. Saga borrows the "Poetic" nature of Ruby, ensuring that code isn't just a set of instructions for a machine, but a readable narrative for the engineer.

**Erlang/BEAM:** For the "Let it Crash" philosophy, process isolation, and the idea of a supervisor-driven runtime. By treating concurrency as a set of isolated "Chapters" with their own lifecycles, Saga brings the legendary uptime and fault tolerance of telecommunications systems to a modern systems language.

**Zig:** For its pragmatic approach to memory. Saga’s memory arenas and error unions were inspired by Zig’s rejection of hidden allocations and its "errors-as-values" model. This ensures that memory management is explicit and efficient without the overhead of a traditional global Garbage Collector.

**LISP:** For tail-call optimization and treating functions as first-class citizens. While Saga is not a purely functional language, it embraces a functional paradigm—allowing for expressive data pipelines and side-effect-managed logic within its procedural structure.

**Rust:** For the concept of "Zero-Cost Abstractions" and memory safety without a borrow checker's cognitive load. Saga looks to Rust's commitment to performance, ensuring that high-level expressiveness never comes at the expense of the underlying machine's efficiency.

**C/C++:** For the "Close to the Metal" transparency. Saga maintains a clear mapping between code and execution, ensuring that a senior engineer can always reason about the generated machine code and memory layout, staying true to the "no magic" principle.
