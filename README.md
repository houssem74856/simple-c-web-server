# Simple C Web Server

Explored Node.js internals by recreating a simplified non-blocking web server in C with a custom event loop and thread pool.

the server relies on Linux’s `epoll` system call, so If you’re on Windows or macOS, it won't work, you will need Docker to run it inside a Linux container,
the "How to Run" section below will cover how to deal with that.

the project has three files, each adding a new layer of functionality:

- `epoll.c`: handles basic connections
- `epoll_echo.c`: adds read/write echo functionality  
- `epoll_threadpool.c`: extends echo with a basic thread pool for concurrent file writes  

---

## How to Run

### 1. Build the Docker image
Open a terminal inside the `simple-c-web-server` folder and run:

```bash
docker build -t simple-c-web-server .
```

This creates a Linux image with everything needed to compile and run the C web servers.

---

### 2. Start the container
Run a container from the image we just created:

```bash
docker run -it --name simple-c-server -p 9000:9000 -v "${PWD}:/workspace" simple-c-web-server
```

You’ll then be inside the Linux shell inside the container.

---

### 3. Compile one of the servers
Inside the container, compile whichever version you want:

```bash
gcc epoll.c -o server_epoll
# or
gcc epoll_echo.c -o server_epoll_echo
# or
gcc epoll_threadpool.c -o server_epoll_threadpool
```
---

### 4. Run the server
```bash
./server_epoll
# or
./server_epoll_echo
# or
./server_epoll_threadpool
```

The web server will start and listen on port **9000**.

---

### 5. Test from Windows
From another Windows terminal (outside the container), send a request using `curl`:

```bash
curl -X POST http://localhost:9000 -d "hello everynyan"
```
---

### 6. Stop the container
When finished, stop the container from another terminal:

```bash
docker stop simple-c-server
```
---
