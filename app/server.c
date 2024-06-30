#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <zlib.h>

#define BUFF_SIZE 1024
#define CHUNK 16384

// Function prototypes
void *process_request(void *socket_fd);
int compress_data(const char *src, size_t src_len, char *dest, size_t dest_len);

// Global variable
char directory[BUFF_SIZE] = ".";

int main(int argc, char *argv[]) {
    // Disable output buffering
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    // Parsing command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--directory") == 0) {
            strncpy(directory, argv[i + 1], sizeof(directory) - 1);
            directory[sizeof(directory) - 1] = '\0';
        }
    }

    // Server setup
    int server_fd, client_addr_len;
    int *client_fd;
    struct sockaddr_in client_addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        printf("Socket creation failed: %s...\n", strerror(errno));
        return 1;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        printf("SO_REUSEADDR failed: %s \n", strerror(errno));
        return 1;
    }

    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(4221),
        .sin_addr = {htonl(INADDR_ANY)},
    };

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        printf("Bind failed: %s \n", strerror(errno));
        return 1;
    }

    int connection_backlog = 5;
    if (listen(server_fd, connection_backlog) != 0) {
        printf("Listen failed: %s \n", strerror(errno));
        return 1;
    }

    while (1) {
        printf("Waiting for a client to connect...\n");
        client_addr_len = sizeof(client_addr);
        client_fd = malloc(sizeof(int));
        if (client_fd == NULL) {
            fprintf(stderr, "Error: Allocating memory for client_fd failed\n");
            return 1;
        }

        *client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        printf("Client connected\n");

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, process_request, (void *)client_fd) < 0) {
            fprintf(stderr, "Error: Could not create thread\n");
            free(client_fd);
            close(*client_fd);
            close(server_fd);
            return 1;
        }

        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}

void *process_request(void *socket_fd) {
    int *client_fd = (int *)socket_fd;
    char buf[BUFF_SIZE];
    int read_bytes = read(*client_fd, buf, BUFF_SIZE);
    printf("msg read - %s\n", buf);

    char method[16], url[512], protocol[16];
    sscanf(buf, "%s %s %s", method, url, protocol);
    printf("URL - %s\n", url);
    int bytes_sent;
    char response[BUFF_SIZE] = {0};

    if (strcmp(method, "POST") == 0) {
        if (strncmp(url, "/files/", 7) == 0) {
            char *file_name = url + 7;
            char file_path[BUFF_SIZE];
            snprintf(file_path, sizeof(file_path), "%s%s", directory, file_name);
            printf("File path - %s\n", file_path);

            char *content_start = strstr(buf, "\r\n\r\n");
            if (content_start != NULL) {
                content_start += 4;
                FILE *fp = fopen(file_path, "wb");
                if (fp != NULL) {
                    fwrite(content_start, 1, read_bytes - (content_start - buf), fp);
                    fclose(fp);
                    snprintf(response, sizeof(response), "HTTP/1.1 201 Created\r\n\r\n");
                } else {
                    snprintf(response, sizeof(response), "HTTP/1.1 500 Internal Server Error\r\n\r\n");
                }
            } else {
                snprintf(response, sizeof(response), "HTTP/1.1 400 Bad Request\r\n\r\n");
            }
        }
    }

    if (strcmp(method, "GET") == 0) {
        snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n200 OK");

        if (strncmp(url, "/files/", 7) == 0) {
            char *file_name = url + 7;
            char file_path[BUFF_SIZE];
            snprintf(file_path, sizeof(file_path), "%s%s", directory, file_name);

            FILE *fp = fopen(file_path, "r");
            if (fp == NULL) {
                fprintf(stderr, "Error: Can not open %s\n", file_path);
                snprintf(response, sizeof(response), "HTTP/1.1 404 Not Found\r\n\r\n\r\n");
            } else {
                char file_buffer[BUFF_SIZE];
                int bytes_read = fread(file_buffer, 1, sizeof(file_buffer) - 1, fp);
                file_buffer[bytes_read] = '\0';
                fclose(fp);

                char compressed_buffer[BUFF_SIZE];
                size_t compressed_size = BUFF_SIZE;
                if (compress_data(file_buffer, bytes_read, compressed_buffer, compressed_size) == Z_OK) {
                    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Encoding: gzip\r\nContent-Length: %ld\r\n\r\n", compressed_size);
                    bytes_sent = send(*client_fd, response, strlen(response), 0);
                    bytes_sent = send(*client_fd, compressed_buffer, compressed_size, 0);
                } else {
                    snprintf(response, sizeof(response), "HTTP/1.1 500 Internal Server Error\r\n\r\n");
                }
            }
        } else if (strcmp(url, "/") == 0) {
            snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\n\r\n");
        } else if (strncmp(url, "/echo/", 6) == 0) {
            char *echo = url + 6;
            char *encoding = strstr(buf, "Accept-Encoding:");
            if (encoding != NULL) {
                encoding += 16;
                if (strstr(encoding, "gzip") != NULL) {
                    char compressed_buffer[BUFF_SIZE];
                    size_t compressed_size = BUFF_SIZE;
                    if (compress_data(echo, strlen(echo), compressed_buffer, compressed_size) == Z_OK) {
                        snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Encoding: gzip\r\nContent-Length: %ld\r\n\r\n", compressed_size);
                        bytes_sent = send(*client_fd, response, strlen(response), 0);
                        bytes_sent = send(*client_fd, compressed_buffer, compressed_size, 0);
                    } else {
                        snprintf(response, sizeof(response), "HTTP/1.1 500 Internal Server Error\r\n\r\n");
                    }
                } else {
                    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %lu\r\n\r\n%s", strlen(echo), echo);
                }
            } else {
                snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %lu\r\n\r\n%s", strlen(echo), echo);
            }
        } else if (strncmp(url, "/user-agent", 11) == 0) {
            char *user_agent = strstr(buf, "User-Agent:");
            if (user_agent != NULL) {
                user_agent += 12;
                char *eol = strstr(user_agent, "\r\n");
                if (eol != NULL) {
                    *eol = '\0';
                }
            } else {
                user_agent = "User-agent not found";
            }
            snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %lu\r\n\r\n%s", strlen(user_agent), user_agent);
        } else {
            snprintf(response, sizeof(response), "HTTP/1.1 404 Not Found\r\n\r\n\r\n");
        }
    }

    bytes_sent = send(*client_fd, response, strlen(response), 0);
    close(*client_fd);
    free(client_fd);
    return NULL;
}

int compress_data(const char *src, size_t src_len, char *dest, size_t dest_len) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return Z_ERRNO;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = (Bytef *)dest;
    strm.avail_out = (uInt)dest_len;

    int ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        return Z_ERRNO;
    }

    dest_len = strm.total_out;
    deflateEnd(&strm);
    return Z_OK;
}

