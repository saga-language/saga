# Saga programming language

Like Go but with the soul of Ruby and the resilience of Erlang. Saga is a compiled, statically-typed language designed for senior engineers who love the "boring" reliability of Go but miss the expressive joy of Ruby. It bridges the gap between rapid prototyping and bulletproof production systems.

Saga is more than a name; it refers to the resilience of the execution, invoking the Norse Goddess' domain of reasoning and knowledge. Even when elements of the system fail, the program (history) lives on. Saga achieves this through strict memory isolation. Every coroutine operates in its own arena; when a chapter fails, its memory is reclaimed without side effects, allowing the supervisor to reschedule it from a clean state.

- [Language specification](docs/language.md)
- [Standard Library](docs/stdlib.md)
- [Runtime](docs/runtime.md)
- [Tools](docs/tools.md)
- [Formal grammar](docs/grammar.md)
- [Concurrency and memory model](docs/concurrency.md)
- [Influences](docs/influences.md)

## Key Features

**Fail-Tolerant Concurrency:** Saga treats coroutines as isolated units of work. Using memory arenas and resource quotas, a crashed coroutine is a managed event, not a process-ending catastrophe. It brings the "Let it Crash" philosophy of the Actor model to a familiar procedural syntax.

**Elegant Error Handling:** Saga replaces repetitive error-checking boilerplate with expressive, union-based error handling. The or block ensures you acknowledge failures without losing your flow—safety that feels like Ruby but performs like C.

**High-Velocity Systems:** Built for the engineer who needs the "Get it done" speed of Ruby on Rails without the technical debt of a dynamic runtime. With built-in loop accumulators and natural indexing, Saga stays out of your way so you can focus on the problem, not the plumbing.

## Installation

N/A

## Getting started

No language is complete without the obligatory "Hello, world!" example.

```sg
import "io"

pub fn Main() Void {
  io.Println("Hello, world!")
}
```

## Contributing

Saga is not currently accepting contributions. Once the initial implementation is complete, contributions will
be welcome.
