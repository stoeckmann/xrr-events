//compile with: g++ xrr-events.cpp -oxrr-events -Wall -Weffc++ -lX11 -lXrandr
//TODO load options from a config file
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <deque>
#include <getopt.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#define VERSION "0.1"

#define SCRIPT_FILENAME "event.sh"
#define PID_FILENAME "xrr-events.pid"
//includes trailing \0
#define PID_STR_LENGTH 20
#define LOG_LEVEL_ALL 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_ERROR 3

class Application;
void handle_signal(int signo);

//can't pass user data to a signal
Application *app_for_sighandler = NULL;

typedef std::deque<std::string> Args;

bool makedirs(const std::string &path, mode_t mode=0744) {
    std::string path_copy(path);
    size_t len = path_copy.size();
    size_t pos;

    if (path_copy[0] == '/')
        pos = 1;
    else
        pos = 0;

    for (; pos < len; ++pos) {
        if (path_copy[pos] == '/') {
            path_copy[pos] = '\0';
            if (mkdir(path_copy.c_str(), mode) < 0) {
                if (errno != EEXIST)
                    return false;
            }
            path_copy[pos] = '/';
        }
    }

    if (mkdir(path_copy.c_str(), mode) < 0) {
        if (errno != EEXIST)
            return false;
    }

    return true;
}

/* logging stuff *//*{{{*/
unsigned char show_log_level = LOG_LEVEL_ALL;

const char *log_level_to_string(int n) {
    switch (n) {
        case LOG_LEVEL_DEBUG:
            return "debug";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_ERROR:
            return "error";
    }
    return "unknown";
}

void vlog(unsigned char level, const char *filename,
        const char *function, unsigned int lineno,
        const char *fmt, va_list args) {
    //TODO
    //YYYY-MM-DD/HH:MM:SS
    //char timebuf[20];
    //strftime(timebuf, 20, "",
    printf("[%s:%s:%d/%s] ", filename, function, lineno, log_level_to_string(level));
    vfprintf(stdout, fmt, args);
}

void log(unsigned char level, const char *filename,
        const char *function, unsigned int lineno,
        const char *fmt, ...) {
    if (level < show_log_level) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vlog(level, filename, function, lineno, fmt, args);
    printf("\n");
    va_end(args);
}

void _log_error_unix(unsigned char level, const char *filename,
        const char *function, unsigned int lineno,
        const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level, filename, function, lineno, fmt, args);
    printf(": %s\n", strerror(errno));
    va_end(args);
}

#define log_debug(fmt, ...) log(LOG_LEVEL_DEBUG, __FILE__, __FUNCTION__, __LINE__, fmt, ## __VA_ARGS__);
#define log_error(fmt, ...) log(LOG_LEVEL_ERROR, __FILE__, __FUNCTION__, __LINE__, fmt, ## __VA_ARGS__);
#define log_error_unix(fmt, ...) _log_error_unix(LOG_LEVEL_ERROR, __FILE__, __FUNCTION__, __LINE__, fmt, ## __VA_ARGS__);
#define log_info(fmt, ...) log(LOG_LEVEL_INFO, __FILE__, __FUNCTION__, __LINE__, fmt, ## __VA_ARGS__);
/*}}}*/
const char *connection_to_string(Connection c) {
    switch (c) {
        case RR_Connected:
            return "Connected";
        case RR_Disconnected:
            return "Disconnected";
        case RR_UnknownConnection:
            return "Unknown";
    }
    return "??";
}

static void usage(void) {
    printf("xrr-events [options]\n"
           "\t--replace : Kill current instance and replace it\n"
           "\t--kill : Kill current instance and exit\n"
           "\t--log-level=LEVEL : Only output messages greater than the given log level\n"
           "\t--script-file=FILENAME : Use the given file as the event script\n"
           "\t--daemonize : Run in background\n"
           "\t--version : Print version and exit\n"
           "\t--help : Display this message and exit\n");
}

std::string path_join(const std::string &a, const std::string &b) {
    return a + "/" + b;
}

enum AppUserModeType {
    UserMode,
    SystemMode
};

struct ApplicationPaths {/*{{{*/
    std::string toplevel_dir;
    std::string cache_dir;
    std::string log_dir;

    std::string pid_file_path;
    std::string log_path;
    std::string err_log_path;
    std::string script_path;

    ApplicationPaths(AppUserModeType mode) :
        toplevel_dir(), cache_dir(), log_dir(),
        pid_file_path(), log_path(), err_log_path(),
        script_path()
    {
        if (mode == UserMode) {
            char *home_dir = getenv("HOME");
            if (!home_dir)
                throw std::runtime_error("Attempting to run as a user but no HOME envar set");

            toplevel_dir = path_join(path_join(home_dir, ".config"), "xrr-events");
            cache_dir = path_join(home_dir, ".cache");
            log_dir = path_join(toplevel_dir, "log");

        }
        else if (mode == SystemMode) {
            toplevel_dir = "/";
            cache_dir = "/var/run";
            log_dir = "/var/log";
        }
        else
            throw std::runtime_error("Invalid run mode given");

        pid_file_path = path_join(cache_dir, PID_FILENAME);
        script_path = path_join(toplevel_dir, SCRIPT_FILENAME);
        log_path = path_join(log_dir, "stdout.log");
        err_log_path = path_join(log_dir, "stderr.log");
    }

    bool create_dirs(void) {
        if (!makedirs(toplevel_dir)) {
            log_error_unix("makedirs failed for %s", toplevel_dir.c_str());
            return false;
        }

        if (!makedirs(log_dir)) {
            log_error_unix("makedirs failed for %s", log_dir.c_str());
            return false;
        }

        if (!makedirs(cache_dir)) {
            log_error_unix("makedirs failed for %s", cache_dir.c_str());
            return false;
        }

        return true;
    }
};/*}}}*/

class PidFile {/*{{{*/
    private:
        PidFile(const PidFile &);
        PidFile &operator=(const PidFile &);

        std::string pid_path;
        pid_t pid;
        bool wrote_pid;

    public:
        PidFile(const std::string &pid_path) :
            pid_path(pid_path), pid(-1),
            wrote_pid(false)
        {}

        ~PidFile(void) {
            if (wrote_pid)
                remove_file();
        }

        pid_t get_pid(void) {
            return pid;
        }

        /**
         * Returns -1 on error, -2 if pidfile doesn't exist, or the the pid_t
         *
         * A successful call to store_pid() will cause this to return our pid unless
         * force_read is set; else, calling this will read the file and return the
         * pid inside (and will always return this unless another call to store_pid()
         * is made)
         */
        pid_t read_pid_from_file(bool force_read=false) {
            if (pid > 0 && !force_read)
                return pid;

            FILE *pidf;

            errno = 0;
            if (!(pidf = fopen(pid_path.c_str(), "rb"))) {
                if (errno == ENOENT)
                    return -2;
                log_error_unix("Unable to open pid file %s", pid_path.c_str());
                return -1;
            }

            //XXX longest possible 64bit decimal string (find a decent way to test this)
            char buf[PID_STR_LENGTH] = "";
            errno = 0;
            fread(buf, PID_STR_LENGTH-1, 1, pidf);
            if (errno) {
                log_error_unix("Unable to read from pid file");
                fclose(pidf);
                return -1;
            }
            fclose(pidf);

            errno = 0;
            pid = strtol(buf, NULL, 10);
            if (errno != 0) {
                pid = -1;
                log_error_unix("Unable to convert pid read from file");
                return -1;
            }

            return pid;
        }

        /**
         * Checks if a pid file exists. If it does, check if the proc is still
         * running. If it is, return true. Else, if pidfile exists and proc is dead,
         * delete file and return false. If no pidfile, return false
         *
         * Note: may call abort()
         */
        bool is_running(void) {
            pid_t pid = read_pid_from_file();
            if (pid == -1)
                abort();
            else if (pid == -2)
                return false;

            log_info("Found pidfile with pid=%d", pid);
            char path[PID_STR_LENGTH+6];
            snprintf(path, 26, "/proc/%d", pid);

            struct stat kiyoka;

            errno = 0;
            if (stat(path, &kiyoka) < 0) {
                if (errno == ENOENT) {
                    remove_file();
                    return false;
                }
                log_error_unix("Unable to stat() %s", path);
                abort();
            }

            if (!S_ISDIR(kiyoka.st_mode)) {
                log_error("%s isn't a directory", path);
                return false;
            }

            return true;
        }

        /**
         * Removes existing pid file
         *
         * Note may call abort()
         */
        void remove_file(void) {
            if (unlink(pid_path.c_str()) < 0) {
                if (errno == ENOENT)
                    return;
                log_error_unix("Unable to remove pid file");
                abort();
            }
        }

        /**
         * Writes current pid to pidfile
         *
         * Note may call abort()
         */
        void store_pid(void) {
            pid_t our_pid = getpid();
            FILE *fd;
            if (!(fd = fopen(pid_path.c_str(), "wb"))) {
                log_error_unix("Unable to open %s for writing", pid_path.c_str());
                abort();
            }
            char buf[PID_STR_LENGTH];
            snprintf(buf, PID_STR_LENGTH, "%d", our_pid);
            if (fwrite(buf, strlen(buf), 1, fd) != 1) {
                log_error_unix("Unable to write pid to file");
                abort();
            }
            fclose(fd);
            wrote_pid = true;
            pid = our_pid;
        }
};/*}}}*/

void do_exec(std::string path, const Args &args) {
    int argc = args.size()+2;
    char **argv = new char *[argc];
    int l = args.size();

    argv[0] = const_cast<char *>(path.c_str());
    argv[argc-1] = NULL;
    for (int i=0; i < l; ++i) {
        //since this'll never return, don't bother dup'ing these strings
        argv[i+1] = const_cast<char *>(args[i].c_str());
    }

    if (execv(path.c_str(), argv) < 0) {
        log_error("execv failed, aborting");
        abort();
    }
}

//just fork and run the given program
pid_t fork_and_exec(std::string path, const Args &args) {
    pid_t pid;
    switch ((pid = fork())) {
        //child
        case 0:
            do_exec(path, args);
            return -1;
        //error to parent
        case -1:
            return -1;
    }
    //parent
    return pid;
}

class Application {/*{{{*/
    private:
        //x11
        Display *display;
        //xrr
        XRRScreenResources *resources;
        int xrr_event_base;
        int xrr_error_base;
        int xrr_major_ver;
        int xrr_minor_ver;

        bool skip_next_event;
        bool quit_app;
        ApplicationPaths paths;

        //options
        bool daemonize;
        bool do_kill;
        bool do_replace;

        Application(const Application &);
        Application &operator=(const Application &);

    public:
        Application(const ApplicationPaths &paths)
            : display(NULL), resources(NULL),
            xrr_event_base(0), xrr_error_base(0),
            xrr_major_ver(0), xrr_minor_ver(0),
            skip_next_event(false), quit_app(false), paths(paths),
            daemonize(false), do_kill(false), do_replace(false)
        {
            ;
        }

        ~Application(void) {
            if (resources)
                XRRFreeScreenResources(resources);
            if (display) {
                XRemoveConnectionWatch(display,
                        Application::static_x_connection_added, (XPointer)this);
                XCloseDisplay(display);
            }
        }

        /**
         * Returns true if we should exit afterwards (--kill), or wait then attempt
         * to run (--replace)
         */
        bool kill_running_instance(pid_t pid) const {
            if (kill(pid, SIGTERM) < 0) {
                log_error_unix("Unable to kill pid=%d", pid);
                return false;
            }
            return true;
        }

        /*
         * Initialize X and XRandR
         */
        bool init(void) {
            if (!(display = XOpenDisplay(NULL))) {
                log_error("XOpenDisplay");
                return false;
            }

            if (!XRRQueryExtension(display, &xrr_event_base, &xrr_error_base)) {
                log_error("XRRQueryExtension");
                return false;
            }

            if (!XRRQueryVersion(display, &xrr_major_ver, &xrr_minor_ver)) {
                log_error("XRRQueryVersion");
                return false;
            }

            log_info("XRandR version %d.%d", xrr_major_ver, xrr_minor_ver);

            int default_screen = XDefaultScreen(display);
            Window root = XRootWindow(display, default_screen);

            if (!(resources = XRRGetScreenResources(display, root))) {
                log_error("XRRGetScreenResources");
                return false;
            }

            XRRSelectInput(display, root, RROutputChangeNotifyMask);

            //X can open other connections to the display, this lets us be
            //notified of them
            if (!XAddConnectionWatch(display,
                        Application::static_x_connection_added, (XPointer)this)) {
                log_error("XAddConnectionWatch");
                return false;
            }

            //setup signal handling stuff
            app_for_sighandler = this;
            signal(SIGINT, ::handle_signal);
            signal(SIGTERM, ::handle_signal);

            return true;
        }

        /**
         * Note: may set global logfile variable, may call exit
         */
        void parse_args(int argc, char **argv) {
            struct option opts[] = {
                { "replace", no_argument, NULL, 'r' },
                { "kill", no_argument, NULL, 'k' },
                //will show messages >n
                { "log-level", required_argument, NULL, 'l' },
                { "script-file", required_argument, NULL, 's' },
                { "daemonize", no_argument, NULL, 'd' },
                { "version", no_argument, NULL, 'v' },
                { "help", no_argument, NULL, 'h' },
            };
            const char *optstr = "flsdh";
            char c;

            while ((c = getopt_long_only(argc, argv, optstr, opts, NULL)) != -1) {
                switch (c) {
                    case 'r':
                        do_replace = true;
                        break;
                    case 'k':
                        do_kill = true;
                        break;
                    case 'l':
                        errno = 0;
                        show_log_level = strtol(optarg, NULL, 10);
                        if (errno != 0) {
                            log_error_unix("Unable to convert %s to an int", optarg);
                            show_log_level = 0;
                        }
                        break;
                    case 's':
                        paths.script_path = optarg;
                        break;
                    case 'd':
                        daemonize = true;
                        break;
                    case 'h':
                        usage();
                        exit(0);
                        break;
                    case 'v':
                        printf("xrr-events %s\n", VERSION);
                        exit(0);
                        break;
                    default:
                        printf("unknown\n");
                        break;
                }
            }
        }

        void handle_signal(int signo) {
            if (signo == SIGINT || signo == SIGTERM)
                quit_app = true;
        }

        void daemonize_if_set(void) const {
            if (daemonize) {
                log_info("Entering daemon mode");
                daemonize_or_die();
            }
        }

        void daemonize_or_die(void) const {
            pid_t pid;
            if ((pid = fork()) < 0) {
                log_error_unix("fork failed");
                abort();
            }
            else if (pid != 0)
                exit(0);

            setsid();

            //XXX at this point we have no controlling terminal so all these logging calls are pointless

            if ((pid = fork()) < 0) {
                log_error_unix("Second fork failed");
                abort();
            }
            else if (pid != 0)
                exit(0);

            if (chdir("/") < 0) {
                log_error_unix("Unable to chdir to /");
                abort();
            }

            umask(0);

            close(STDIN_FILENO);

            if (!freopen(paths.log_path.c_str(), "wb", stdout)) {
                log_error_unix("freopen for stdout failed");
                abort();
            }

            if (!freopen(paths.err_log_path.c_str(), "wb", stderr)) {
                log_error_unix("freopen for stderr failed");
                abort();
            }
        }

        int run(void) {
            PidFile f(paths.pid_file_path);

            if (f.is_running()) {
                if (do_kill || do_replace) {
                    log_info("Sending SIGTERM to current instance");

                    if (!kill_running_instance(f.get_pid()))
                        return -1;
                    if (!do_replace)
                        return 0;

                    log_info("Waiting for process to terminate");
                    sleep(1);

                    if (f.is_running()) {
                        log_error("Process still running");
                        return -1;
                    }
                }
                else {
                    log_info("Already running, aborting execution");
                    return -1;
                }
            }
            else if (do_kill) {
                log_info("No running instance to kill");
                return 0;
            }

            daemonize_if_set();
            f.store_pid();

            return mainloop();
        }

        int mainloop(void) {
            XEvent ev;
            log_debug("Entering mainloop");

            struct pollfd fds[1];
            fds->fd = XConnectionNumber(display);
            fds->events = POLLIN;

            while (!quit_app) {
                //check if we have any pending events, if so, process them
                while (XPending(display)) {
                    log_debug("Waiting for event");
                    XNextEvent(display, &ev);
                    log_debug("Received an event");

                    XRRUpdateConfiguration(&ev);

                    if (skip_next_event) {
                        log_debug("Skipping event");
                        skip_next_event = false;
                        continue;
                    }

                    switch (ev.type-xrr_event_base) {
                        case RRNotify:
                            handle_notify_event((XRRNotifyEvent *)&ev);
                            break;
                    }
                }

                fflush(stdout);

                //make sure we send out any outstanding requests in the local cache
                //before we wait for changes
                XFlush(display);

                log_debug("Running poll()");
                if (poll(fds, 1, -1) < 0) {
                    if (errno == EINTR) {
                        log_debug("poll interrupted by signal");
                        continue;
                    }
                    log_error_unix("poll() failed");
                    break;
                }
                XProcessInternalConnection(display, fds[0].fd);
                log_debug("Poll returned");
            }
            return 0;
        }

        void handle_notify_event(XRRNotifyEvent *ev) {
            if (ev->subtype != RRNotify_OutputChange) {
                log_error("Received an unknown XRRNotifyEvent subtype: %d", ev->subtype);
                return;
            }
            //has .mode:RRMode for the currently set mode, or None
            XRROutputChangeNotifyEvent *oev = (XRROutputChangeNotifyEvent *)ev;
            XRROutputInfo *output_info = XRRGetOutputInfo(display, resources, oev->output);
            const char *conn_state = connection_to_string(output_info->connection);
            log_info("Output changed: name=%s; connection=%s",
                    output_info->name, conn_state);
            Args args;
            args.push_back(output_info->name);
            args.push_back(conn_state);
            run_script(args);
            XRRFreeOutputInfo(output_info);
        }

        //TODO check if output has an associated mode (use XRROutputChangeNotifyEvent.mode)
        void run_script(Args args) {
            struct stat kiyoka;

            if (stat(paths.script_path.c_str(), &kiyoka) < -1) {
                log_error_unix("Unable to run stat(script)");
                return;
            }
            if (!(kiyoka.st_mode & S_IXUSR)) {
                log_error("Script isn't executable");
                return;
            }

            log_info("Running %s", paths.script_path.c_str());

            pid_t pid = fork_and_exec(paths.script_path, args);
            if (pid < 0) {
                log_error_unix("fork_and_exec failed");
            }
            int status;

            if (waitpid(pid, &status, 0) < 0) {
                log_error_unix("waitpid failed");
                return;
            }

            if (WIFEXITED(status)) {
                status = WEXITSTATUS(status);
                if (status < 0)
                    log_error("Script failed; returned %d", status);
                log_info("Script completed");
                if (status > 0)
                    skip_next_event = true;
            }

            else if (WIFSIGNALED(status)) {
                log_error("Script was terminaled by signal=%d", WTERMSIG(status));
            }

            else if (WIFSTOPPED(status)) {
                log_error("Script was stopped by signal=%d", WSTOPSIG(status));
            }
            else
                log_error("Unknown status: %d\n", status);
        }

        /* callbacks */
        //TODO fix this
        void x_connection_added(int fd, Bool opening) {
            if (opening) {
                log_debug("Adding new fd %d\n", fd);
            }
            else {
                log_debug("Removing fd %d\n", fd);
            }
        }

        /* static wrappers for callback functions */
        static void static_x_connection_added(Display *display, XPointer user_data,
                int fd, Bool opening, XPointer *watch_data) {
            Application *app = (Application *)user_data;
            app->x_connection_added(fd, opening);
        }
};/*}}}*/

/**
 * Call the registered Application's signal handler
 */
void handle_signal(int signo) {
    if (!app_for_sighandler)
        return;
    app_for_sighandler->handle_signal(signo);
}

int main(int argc, char **argv) {
    ApplicationPaths paths(UserMode);

    if (!paths.create_dirs())
        return -1;

    Application app(paths);

    app.parse_args(argc, argv);

    if (!app.init()) {
        log_error("Application initilization failed");
        return -1;
    }

    app.run();
}