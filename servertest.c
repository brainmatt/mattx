#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char *hello = "Hello from the MattX Surrogate!\n";

    printf("[Server %d] Starting up...\n", getpid());

    // 1. Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return 1;
    }
    printf("[Server %d] Socket created.\n", getpid());

    // 2. Bind
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return 1;
    }
    printf("[Server %d] Bound to port %d.\n", getpid(), PORT);

    // 3. Listen
    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        return 1;
    }
    
    printf("[Server %d] Listening... MIGRATE ME NOW, THEN CONNECT TO VM1:8080!\n", getpid());

    // 4. Accept (This will block and wait for the Wormhole!)
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("Accept failed");
        return 1;
    }

    // 5. Success! Print client IP to prove the sockaddr copy worked
    printf("[Server %d] SUCCESS! Connection accepted from %s:%d!\n", 
           getpid(), inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // 6. Write to the client (This will use our VFS Fake FD Proxy!)
    write(client_fd, hello, strlen(hello));
    printf("[Server %d] Message sent. Exiting.\n", getpid());

    close(client_fd);
    close(server_fd);
    return 0;
}

