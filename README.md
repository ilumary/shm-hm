# Shared Memory Demo

Demo program to show off POSIX shared memory communication and a (very simple) threadsafe HashMap. Clients can enqueue requests (read, delete, get) into shared memory and the server serves them via multiple threads and puts the result back into the shared memory. The HashMaps size is set at launch. Collisions are resolved by a linked list. The HashMap is fully threadsafe.

All design choices, assumptions, tradeoffs, and the evaluation can be found in `ARCHITECTURE.md`.

## Compile & Run

Compile with

```sh
make
```

Start server with

```sh
./build/release/server [buckets] [threads]
```

Start client with

```sh
./build/release/client
```

The client requires commands to do things. You can find examples in `client_test_input_*.txt` files. A detailed explanation is given in `ARCHITECTURE.md` under the `Client` section.
