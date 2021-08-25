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

int run(sd_journal *journal)
{
    if (sd_journal_seek_tail(journal) < 0) {
        perror("Failed to seek to the end of system journal");
        return errno;
    }

    const int history = 20; // Scroll back 20 messages
    for (int i = 0; i < history; i++) {
        if (sd_journal_previous(journal) < 0) {
            perror("Failed to move backwards in journal");
            return errno;
        }
    }

    while (true) {
        const int type = sd_journal_wait(journal, -1lu);

        if (type < 0) {
            printf("Failed to process wait for journal event: %d (%s)\n", type, strerror(-type));
            return -type;
        }

        switch(type) {
        case SD_JOURNAL_NOP:
            continue;
        case SD_JOURNAL_INVALIDATE:
            // We might have missed some events, but it seems spurious
            // The documentation suggests treating it like SD_JOURNAL_APPEND
        case SD_JOURNAL_APPEND:
            while (sd_journal_next(journal)) {
                print_journal_message(journal);
            }
            continue;
        default:
            printf("Unhandled type %d\n", type);
            break;
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
        return -ret;
    }
    ret = run(journal);
    sd_journal_close(journal);

    return ret;
}
