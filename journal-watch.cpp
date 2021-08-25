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

enum LogLevel {
    Emergency = 0,
    Alert = 1,
    Critical = 2,
    Error = 3,
    Warning = 4,
    Notice = 5,
    Informational = 6,
    Debug = 7
};
namespace Color {
    const char *brightGray = "\033[00;37m";

    const char *white = "\033[00;39m";
    const char *brightWhite = "\033[01;39m";

    const char *blue = "\033[00;34m";
    const char *brightBlue = "\033[01;34m";

    const char *green = "\033[00;32m";
    const char *brightGreen = "\033[01;32m";

    const char *yellow = "\033[00;93m";
    const char *brightYellow = "\033[01;33m";

    const char *orange = "\033[00;33m";
    const char *red = "\033[00;31m";
    const char *brightRed = "\033[00;101m";

    const char *reset = "\033[0m";
};

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
        if (-ret == ENOENT) { // Field does not exist
            return "";
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

    int level = Debug;
    try {
        level = std::stoi(fetchField(j, "PRIORITY"));
    } catch (const std::exception &) {}

    const char *color = Color::white;
    switch(level) {
    case Emergency:
        color = Color::brightRed;
        break;
    case Alert:
        color = Color::red;
        break;
    case Critical:
        color = Color::orange;
        break;
    case Error:
        color = Color::brightYellow;
        break;
    case Warning:
        color = Color::yellow;
        break;
    case Notice:
        color = Color::green;
        break;
    case Informational:
        color = Color::white;
        break;
    case Debug:
    default:
        color = Color::brightGray;
        break;
    }

    time_t sec = usec / 1000000;
    std::tm tm;
    localtime_r(&sec, &tm);
    std::cout << "\033[02;37m"
        << std::put_time(&tm, "%H:%M:%S %b %d ")
        << fetchField(j, "_HOSTNAME");

    std::string uid = fetchField(j, "_UID");
    if (uid.empty()) {
        uid = fetchField(j, "_AUDIT_LOGINUID");
    }

    if (!uid.empty()) {
        std::cout << ":" << getUsername(uid);
    }

    std::string identifier = fetchField(j, "SYSLOG_IDENTIFIER");
    if (identifier.empty()) {
        identifier = fetchField(j, "_COMM");
    }
    std::cout << " " << identifier;

    const std::string pid = fetchField(j, "_PID");
    if (!pid.empty()) {
        std::cout << "[" << pid << "]";
    }

    std::cout << ": "
        << color
        << fetchField(j, "MESSAGE")
        << Color::reset
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

    const int history = 20;

    /* Tail -f displays last 10 messages */
    for (int i = 0; i < history; i++) {
        if (sd_journal_previous(*journal) < 0) {
            perror("Failed to move backwards in journal");
            return errno;
        }
    }

    for (int i = 0; i < history; i++) {
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

    uint64_t timeout = -1lu;

    while (true) {
        sd_journal_get_timeout(*journal, &timeout);
        if (timeout != -1lu) {
            timeout /= 1000; // sd_journal_get_timeout returns microseconds, epoll uses milliseconds
        } else {
            timeout = 1000; // 1s by default
        }

        struct epoll_event e = {};
        int events = epoll_wait(epoll_fd, &e, 1, timeout);

        if (errno == EINTR) {
            continue;
        }

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

            const int ret = sd_journal_open(journal, SD_JOURNAL_LOCAL_ONLY);
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

    if (geteuid() != 0) {
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
