# Architecture

The project consists of two programs that talk to each other through POSIX shared memory:

- server: owns a thread-safe hash table and serves requests with a pool of worker threads.
- client: reads commands from stdin and sends them to the server as requests (insert, get, delete, dump bucket).

### Language and platform

C++23 and Make. Needs GCC 14 or Clang 19 or newer. Linux only. Built with the Makefile (`make` or `make debug`).

### Scope

The project is deliberately kept small. Several limitations are known and accepted. They are listed in the Evaluation section together with how they could be fixed.

## HashMap

### Design

- Buckets. The number of buckets is passed on the command line and fixed for the lifetime of the server. The bucket array is allocated once on the heap.
- Collision resolution. Each bucket is a `std::forward_list<Node>` (a singly linked list), where a node is a key (`std::uint64_t`) and a value (`std::vector<std::byte>`).
- Hashing. `index = fmix64(key) % buckets`, where `fmix64` is the 64-bit mixer from [MurmurHash3](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp). The mixing function spreads every input bit over the whole space.
- Operations.
  - `insert(key, value)`: if the key exists, its value is overwritten. Otherwise a node is added at the front of the list.
  - `get(key)`: returns a copy of the value, or `std::nullopt`.
  - `remove(key)`: returns whether something was deleted.
  - `dump_bucket(index)`: returns a copy of all key-value pairs in one bucket, or `std::nullopt` if the bucket is empty.

### Locking

Every bucket has its own `std::shared_mutex`:

- `get` and `dump_bucket` take a shared lock, so any number of readers can work on the same bucket at once.
- `insert` and `remove` take an exclusive lock.
- Operations on different buckets never block each other.

Because the table never resizes, the bucket array itself never changes, so finding a bucket needs no lock at all. Only the list inside a bucket is protected.

### Trade-offs

- Lock granularity. There are three options: one lock for the whole table, lock striping (M locks shared by N buckets), to one lock per bucket. Per bucket gives the most parallelism but costs memory: on Linux a `std::shared_mutex` is 56 bytes, so a bucket is 64 bytes even when it is empty. With very many buckets, striping would give nearly the same parallelism for a fraction of the memory.
- Duplicate keys overwrite. Inserting an existing key replaces its value (like `map[key] = value`) instead of being rejected.

## SharedMemory

All communication goes through one shared memory segment, `/shm_demo` (visible as `/dev/shm/shm_demo`), about 300 KB with the default parameters. Its layout is a single struct, `ShmLayout`, with four parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `Capacity` | 64 | Request slots in the queue (must be a power of two) |
| `MaxValueSize` | 256 | Maximum value size in bytes |
| `ReplySlots` | 64 | Requests that can be waiting for a reply at the same time, over all clients |
| `MaxEntries` | 16 | Maximum entries in one reply (relevant for bucket dumps) |

### Request queue (clients to server)

A ring buffer of fixed-size request slots. Each request holds an operation, a key, a value (up to `MaxValueSize` bytes) and the index of its reply slot.

- Two counting semaphores: `free_slots` (starts at `Capacity`) and `used_slots` (starts at 0). A writer waits for a free slot, fills it, and posts a used slot. A reader does the opposite.
- Two mutexes guard the write position (`head`) and the read position (`tail`). Writers only contend with writers and readers with readers. The two positions are on separate cache lines to avoid false sharing.

### Reply slots (server to one client)

Replies finish in any order, so they cannot use a second ring buffer. Instead there is a pool of reply slots:

- A stack of free slot IDs, guarded by a mutex, plus a semaphore counting the free IDs. A stack reuses the most recently freed slot first.
- Each reply slot has its own `ready` semaphore and a response: status (`Ok`, `NotFound`, `Error`), a `truncated` flag and up to `MaxEntries` key-value pairs.

### Protocol

- Insert: the client enqueues the request without a reply slot and returns immediately.
- Get, Delete, DumpBucket:
  1. The client takes a free reply slot before enqueueing the request.
  2. The client enqueues the request with the slot ID and gets a `Future`.
  3. A server thread takes the request, handles it, writes the answer into the reply slot through a `Reply` object and posts `ready`.
  4. `Future::get()` waits on `ready`, copies the response out and returns the slot to the free stack.

`Future` and `Reply` protect the slots:

- A `Future` destroyed without `get()` still waits for the reply before freeing its slot. Otherwise the server could write into a slot that has already been handed to another request.
- A `Reply` destroyed without `send()`, for example on an early return, sends `Error`.

### Trade-offs

- Fixed-size slots. Every request reserves `MaxValueSize` bytes even for a delete. That wastes some space but keeps the ring buffer trivial.
- One shared reply pool instead of per-client queues. Per-client queues (one segment per client) would allow unlimited clients and isolate clients from each other, at the cost of the server mapping, tracking and cleaning up one segment per client.
- Values are limited to `MaxValueSize`. Splitting large values over several slots is possible but would let one large value block all other clients.

## Server

```
./server <buckets> [threads=2]
```

- `buckets` (required): number of hash table buckets, a positive integer.
- `threads` (optional, default 2): number of worker threads, a positive integer.

### Structure

`main` parses the arguments, creates the `HashMap` and the shared memory segment, and starts the worker threads. The server runs indefinetly.

### Worker loop

Every worker runs the same loop: take the next request from the queue and handle it. The queue is safe to read from several threads at once, so no extra coordination is needed.

## Client

```
./client < commands.txt
```

The client reads commands from stdin until EOF, one per line, sends each to the server and prints the reply. Testing a new scenario that way is a lot easier, and several clients can run different command files at the same time.

| Line | Meaning |
|---|---|
| `I <key> <data>` | Insert `<data>` (the rest of the line) under `<key>` |
| `G <key>` | Get the value stored under `<key>` |
| `D <key>` | Delete `<key>` |
| `B <bucket>` | Dump all entries of bucket number `<bucket>` (at most 16) |
| empty line | Ignored |

Keys are unsigned 64-bit integers. In the data of `I`, `\xff` stands for the byte with hex value `ff` and `\\` for a backslash, so binary values can be written: `I 3 \x00\x01\x02\xff binary`.

Example:

```
I 1 hello from client 1
I 2 second value
I 3 \x00\x01\x02\xff binary bytes
G 3
D 2
```

## Evaluation

### Performance

Measured in a development container on my MacBook, so these numbers may differ on native systems:

- Mixed traffic: about 250,000–370,000 requests per second with 8 clients and 4 server threads. Half of these were inserts and half were reads waiting for a reply.
- Inserts: about 600,000 inserts per second.

### Known limitations

1. Clients can deadlock each other through the reply pool. If all reply slots are held by clients that are each waiting for another slot, nobody can collect their replies. It is safe as long as the requests in flight over all clients fit into `ReplySlots`. The client program sends one request at a time, so it is not affected. Fix: give each client a fixed quota of slots when it attaches, so a client can only ever block itself.
2. No ordering guarantee for inserts. Inserts get no reply, and requests are spread over several server threads. So even two inserts of the same key from the same client can be applied in the wrong order, and a read can overtake the insert just before it. Fix: give inserts a reply as well, or send all requests for one key to the same thread.
3. A crashed server leaves the segment behind. A client started afterwards attaches to it and waits forever for a reply. The next server start removes it, as does `rm /dev/shm/shm_demo`. Fix: the server's PID in the segment, which clients check.
4. A crashed client leaks its reply slots until the server restarts. Fix: store the owner's PID per slot, and have the server reclaim slots of processes that no longer exist.
5. Bucket dumps are truncated at 16 entries. Fix: a larger `MaxEntries` (more memory per reply slot), an offset parameter (not a consistent snapshot), or replies in several parts.
6. No graceful shutdown. The server is stopped with Ctrl-C. Blocked clients and threads are not woken up, and the segment is only removed at the next start.

