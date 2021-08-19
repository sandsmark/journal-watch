extern "C" {
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <systemd/sd-journal.h>
} // extern "C"

#include <ctime>
#include <string>
#include <iostream>
#include <iomanip>


static std::string getUsername(const std::string &uidString)
{
    long uid = -1;
    try {
        uid = stol(uidString);
    } catch (const std::exception &) {
        // conversion failed
        return uidString;
    }
    if (uid < 0) {
        return uidString;
    }

    // fuck the _r, we don't need it: no threads and very short lived
    passwd *pw = getpwuid(uid);
    if (!pw) {
        return uidString;
    }
    if (strlen(pw->pw_name) == 0) {
        return uidString;
    }
    return pw->pw_name;
}

static std::string fetchField(sd_journal *journal, const std::string &field)
{
    char *message = nullptr;
    size_t messageLength = 0ULL;
    for (int retries = 0; retries < 10; retries++) {
        const int ret = sd_journal_get_data(journal, field.c_str(), (const void **) &message, &messageLength);
        if (-ret == EAGAIN) {
            continue;
        }

        if (ret < 0) {
            perror(("Failed to fetch field " + field + "(" + strerror(-ret) + ")").c_str());
            return "";
        }

        // + 1 since the message is returned as FIELD=whatwewant
        const size_t fieldLength = field.size() + 1;
        const int textLength = messageLength - fieldLength;

        return std::string(message + fieldLength, textLength);;
    }

    puts(("Timeout fetching field " + field).c_str());
    return "";
}

static int print_journal_message(sd_journal *j)
{
    uint64_t usec;
    int ret = sd_journal_get_realtime_usec(j, &usec);
    if (ret < 0) {
        return ret;
    }

    bool highPriority = false;
    try {
        int priority = std::stoi(fetchField(j, "PRIORITY"));
        if (priority < 6) {
            highPriority = true;
        }
    } catch (const std::exception &) {}

    time_t sec = usec / 1000000;
    std::tm tm;
    localtime_r(&sec, &tm);
    std::cout << "\033[02;37m"
        << std::put_time(&tm, "%H:%M:%S %b %d ")
        << fetchField(j, "_HOSTNAME") << ":"
        << getUsername(fetchField(j, "_UID")) << " "
        << fetchField(j, "_COMM");
    const std::string pid = fetchField(j, "_PID");
    if (!pid.empty()) {
        std::cout << "[" << pid << "]";
    }
    std::cout << ": "
        << (highPriority ? "\033[01;37m" : "\033[0m")
        << fetchField(j, "MESSAGE")
        << "\033[0m"
        << std::endl
    ;

    return 0;
}

int run(sd_journal * * const journal)
{
    if (sd_journal_seek_tail(*journal) < 0) {
        perror("Failed to seek to the end of system journal");
        return errno;
    }

    /* Tail -f displays last 10 messages */
    for (int i = 0; i < 10; i++) {
        if (sd_journal_previous(*journal) < 0) {
            perror("Failed to move backwards in journal");
            return errno;
        }
    }

    for (int i = 0; i < 10; i++) {
        print_journal_message(*journal);
        sd_journal_next(*journal);
    }


    int fd = sd_journal_get_fd(*journal);
    if (fd < 0) {
        perror("Failed to obtain system journal file descriptor");
        return errno;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("Failed to create an epoll instance");
        return errno;
    }

    struct epoll_event ev = {};
    ev.events = sd_journal_get_events(*journal);
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("Failed to add system journal file descriptor to epoll instance");
        return errno;
    }

    uint64_t timeout = -1;
    sd_journal_get_timeout(*journal, &timeout);

    while (true) {
        struct epoll_event e = {};
        int events = epoll_wait(epoll_fd, &e, 1, timeout);

        if (events < 0) {
            perror("epoll_wait() failed");
            return errno;
        }

        if (events == 0) {
            continue;
        }

        int type = sd_journal_process(*journal);

        if (type < 0) {
            perror("Failed to process system journal event");
            return errno;
        }

        if (type == SD_JOURNAL_NOP) {
            continue;
        } else if (type == SD_JOURNAL_APPEND) {
            while (sd_journal_next(*journal)) {
                print_journal_message(*journal);
            }
        } else {
            // Invalidated or something, so reopen
            puts("Log object invalidated, re-opening");

            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);

            // Get timestamp of previous
            sd_journal_previous(*journal);

            uint64_t usec;
            sd_id128_t boot_id;

            bool seek = true;
            if (sd_journal_get_monotonic_usec(*journal, &usec, &boot_id) < 0) {
                perror("Failed to obtain monotonic timestap for current journal entry");
                seek = false;
            }

            sd_journal_close(*journal);

            const int ret = sd_journal_open(journal, SD_JOURNAL_SYSTEM | SD_JOURNAL_CURRENT_USER | SD_JOURNAL_LOCAL_ONLY);
            if (ret < 0) {
                perror("Failed to open system journal");
                exit(EXIT_FAILURE);
            }

            if (seek) {
                const int ret = sd_journal_seek_monotonic_usec(*journal, boot_id, usec);

                if (ret < 0) {
                    perror("Failed to seek to last seen entry");
                    sd_journal_seek_tail(*journal);
                }
            }

            fd = sd_journal_get_fd(*journal);
            if (fd < 0) {
                perror("Failed to obtain system journal file descriptor");
                return errno;
            }

            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                perror("Failed to add system journal file descriptor to epoll instance");
                return errno;
            }
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (getuid() != 0) {
        puts("Not running as root, will only print user journal");
    }

    sd_journal *journal;
    int ret = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY);

    if (ret < 0) {
        perror("Failed to open system journal");
        return EXIT_FAILURE;
    }
    ret = run(&journal);
    if (journal) {
        sd_journal_close(journal);
    }

    return EXIT_FAILURE;
}
