# Concurrent Client-Server Expense Management System

A concurrent expense management system inspired by Splitwise, developed in **C** using **POSIX Threads**, **TCP/IP Socket Programming**, and **System V IPC**. The system supports secure multi-user expense management with role-based authentication, real-time synchronization, smart debt simplification, and persistent storage while ensuring thread-safe and deadlock-free execution.

---

## Features

- Multi-client concurrent client-server architecture
- Secure user authentication with session tokens
- Role-Based Access Control (Admin, User, Guest)
- Smart debt simplification for optimized settlements
- Persistent storage using binary files
- Real-time balance synchronization
- Audit logging of user activities
- Deadlock-free and race-condition-free synchronization
- Custom expense splitting between multiple users
- Thread-safe expense management

---

## Technologies Used

- C
- POSIX Threads (pthreads)
- TCP/IP Socket Programming
- Shared Memory
- Message Queues
- Named Semaphores
- POSIX File Locking (`fcntl`)
- Linux System Programming
- Makefile

---

## Project Structure

- `server.c` – Server implementation
- `client.c` – Client application
- `auth.h` – Authentication and session management
- `expense.h` – Expense management logic
- `ipc.h` – Shared memory, semaphores and message queues
- `filelock.h` – POSIX file locking
- `common.h` – Shared data structures and constants
- `Makefile` – Build automation
- `data/` – Runtime storage

## Build and Run

Build the project:

```bash
make
```

Start the server:

```bash
./server
```

Open a new terminal and start the client:

```bash
./client
```

## Default Login

The server automatically creates a default administrator account on first launch.

- **Username:** `admin`
- **Password:** `admin123`

## Key Concepts Demonstrated

- Concurrent client-server programming
- POSIX Threads and mutex synchronization
- TCP/IP socket programming
- Shared Memory, Message Queues and Named Semaphores
- Role-Based Access Control (RBAC)
- Session-based authentication
- Persistent binary file storage
- Smart debt settlement algorithm
