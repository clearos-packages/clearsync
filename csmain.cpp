// ClearSync: system synchronization daemon.
// Copyright (C) 2011-2012 ClearFoundation <http://www.clearfoundation.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <sstream>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <unistd.h>
#include <getopt.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <signal.h>
#include <expat.h>
#include <limits.h>
#include <dirent.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>

#define OPENSSL_THREAD_DEFINES
#include <openssl/opensslconf.h>
#ifndef OPENSSL_THREADS
#error "OpenSSL missing thread support"
#endif
#include <openssl/crypto.h>

#include <clearsync/csexception.h>
#include <clearsync/cslog.h>
#include <clearsync/csconf.h>
#include <clearsync/csevent.h>
#include <clearsync/csutil.h>
#include <clearsync/csthread.h>
#include <clearsync/cstimer.h>
#include <clearsync/csnetlink.h>
#include <clearsync/csplugin.h>

#include "csmain.h"

static pthread_mutex_t **csCryptoMutex = NULL;

static void cs_crypto_lock(int mode, int n, const char *file, int line)
{
    if (csCryptoMutex == NULL) {
        csLog::Log(csLog::Error, "libcrypto mutexes not initialized!");
        return;
    }

    if (mode & CRYPTO_LOCK)
        pthread_mutex_lock(csCryptoMutex[n]);
    else
        pthread_mutex_unlock(csCryptoMutex[n]);
}

void *csSignalHandler::Entry(void)
{
    int sig;
    siginfo_t si;

    csLog::Log(csLog::Debug, "Signal handler started.");

    for ( ;; ) {
        sig = sigwaitinfo(&signal_set, &si);
        if (sig < 0) {
            csLog::Log(csLog::Error, "sigwaitinfo: %s", strerror(errno));
            if (errno == EINTR) {
                usleep(100 * 1000);
                continue;
            }
            EventBroadcast(new csEvent(csEVENT_QUIT,
                csEvent::Sticky | csEvent::HighPriority));
            return NULL;
        }
        csLog::Log(csLog::Debug, "Signal received: %s", strsignal(sig));
        switch (sig) {
        case SIGINT:
        case SIGTERM:
            EventBroadcast(new csEvent(csEVENT_QUIT,
                csEvent::Sticky | csEvent::HighPriority));
            return NULL;

        case SIGHUP:
            EventBroadcast(new csEvent(csEVENT_RELOAD));
            break;

        case SIGCHLD:
            Reaper();
            break;

        default:
            csLog::Log(csLog::Warning,
                "Unhandled signal: %s", strsignal(sig));
        }
    }

    return NULL;
}

void csSignalHandler::Reaper()
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            csLog::Log(csLog::Debug,
                "Process exited with code: %d: %d",
                pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            csLog::Log(csLog::Debug,
                "Process exited by signal: %d: %s",
                pid, strsignal(WTERMSIG(status)));
        }
        else
            csLog::Log(csLog::Warning,
                "Process exited abnormally: %d", pid);
    }
}

csMainXmlParser::csMainXmlParser(void)
    : csXmlParser() { }

void csMainXmlParser::ParseElementOpen(csXmlTag *tag)
{
    csMainConf *_conf = static_cast<csMainConf *>(conf);

    if ((*tag) == "csconf") {
        if (stack.size() != 0)
            ParseError("unexpected tag: " + tag->GetName());
        if (!tag->ParamExists("version"))
            ParseError("version parameter missing");

        _conf->version = atoi(tag->GetParamValue("version").c_str());
        csLog::Log(csLog::Debug,
            "Configuration version: %d", _conf->version);
        if (_conf->version > _CS_CONF_VERSION)
            ParseError("unsupported version, too new");
    }
    else if ((*tag == "plugin")) {
        size_t stack_size = _CS_THREAD_STACK_SIZE;

        if (stack.size() != 0)
            ParseError("unexpected tag: " + tag->GetName());
        if (!tag->ParamExists("name"))
            ParseError("name parameter missing");
        if (!tag->ParamExists("library"))
            ParseError("library parameter missing");
        if (tag->ParamExists("stack-size")) {
            stack_size = (size_t)atol(
                tag->GetParamValue("stack-size").c_str());
            if (stack_size < PTHREAD_STACK_MIN)
                stack_size = PTHREAD_STACK_MIN;
            else if (stack_size % ::csGetPageSize())
                stack_size += (stack_size % ::csGetPageSize());
        }

        map<string, csPluginLoader *>::iterator i;
        i = _conf->parent->plugin.find(tag->GetParamValue("name"));
        if (i != _conf->parent->plugin.end())
            ParseError("duplicate plugin: " + tag->GetParamValue("name"));

        csPluginLoader *plugin = NULL;

        try {
            plugin = new csPluginLoader(
                tag->GetParamValue("library"),
                tag->GetParamValue("name"), _conf->parent, stack_size);
        } catch (csException &e) {
            csLog::Log(csLog::Error, "Plugin loader failed: %s",
                e.estring.c_str());
        }

        if (plugin != NULL) {
            try {
                plugin->GetPlugin()->SetConfigurationFile(_conf->filename);
                tag->SetData(plugin->GetPlugin());
                _conf->parent->plugin[tag->GetParamValue("name")] = plugin;

                csLog::Log(csLog::Debug,
                    "Plugin: %s (%s), stack size: %ld",
                    tag->GetParamValue("name").c_str(),
                    tag->GetParamValue("library").c_str(), stack_size);
            } catch (csException &e) {
                csLog::Log(csLog::Error,
                    "Configuration error: %s: %s: %s",
                    tag->GetParamValue("name").c_str(),
                    e.estring.c_str(), e.what());
                delete plugin;
            }
        }
    }
}

void csMainXmlParser::ParseElementClose(csXmlTag *tag)
{
    string text = tag->GetText();
    csMainConf *_conf = static_cast<csMainConf *>(conf);

    if ((*tag) == "plugin-dir") {
        if (!stack.size() || (*stack.back()) != "csconf")
            ParseError("unexpected tag: " + tag->GetName());
        if (!text.size())
            ParseError("missing value for tag: " + tag->GetName());

        _conf->plugin_dir = text;
        csLog::Log(csLog::Debug,
            "Plug-in configuration directory: %s",
            _conf->plugin_dir.c_str());
    }
    else if ((*tag) == "state-file") {
        if (!stack.size() || (*stack.back()) != "plugin")
            ParseError("unexpected tag: " + tag->GetName());
        if (!text.size())
            ParseError("missing value for tag: " + tag->GetName());

        csPlugin *plugin = reinterpret_cast<csPlugin *>
            (stack.back()->GetData());
        if (plugin != NULL)
            plugin->SetStateFile(text);
    }
    else if ((*tag) == "event-filter") {
        if (!stack.size() || (*stack.back()) != "plugin")
            ParseError("unexpected tag: " + tag->GetName());
        if (!text.size())
            ParseError("missing value for tag: " + tag->GetName());

        csPlugin *plugin = reinterpret_cast<csPlugin *>
            (stack.back()->GetData());
        if (plugin != NULL)
            _conf->parent->ParseEventFilter(plugin, text);
    }
}

csMainConf::csMainConf(csMain *parent,
    const char *filename, csMainXmlParser *parser,
    int argc, char *argv[])
    : csConf(filename, parser, argc, argv),
    parent(parent), version(-1), plugin_dir(_CS_PLUGIN_CONF) { }

csMainConf::~csMainConf() { }

void csMainConf::ScanPlugins(void)
{
    csRegEx regex("\\.conf$", 0, REG_EXTENDED | REG_ICASE); 

    string main_conf_filename = filename;
    size_t dirent_len = offsetof(struct dirent, d_name) +
        pathconf(plugin_dir.c_str(), _PC_NAME_MAX) + 1;
    struct dirent *dirent_entry = (struct dirent *)malloc(dirent_len);

    DIR *dh = opendir(plugin_dir.c_str());
    if (dh == NULL) {
        csLog::Log(csLog::Warning, "Error opening plugin-dir: %s: %s",
            plugin_dir.c_str(), strerror(errno));
        goto ScanPlugins_Leave;
    }

    struct dirent *dirent_result;
    for ( ;; ) {
        if (readdir_r(dh, dirent_entry, &dirent_result) != 0) {
            csLog::Log(csLog::Error, "readdir: %s", strerror(errno));
            break;
        }
        else if (dirent_result == NULL) break;

        if (dirent_result->d_type == DT_DIR) continue;
        else if (dirent_result->d_type != DT_REG &&
            dirent_result->d_type != DT_LNK && dirent_result->d_type != DT_UNKNOWN)
            continue;
        else if (regex.Execute(dirent_result->d_name) == REG_NOMATCH)
            continue;

        try {
            ostringstream os;
            os << plugin_dir << "/" << dirent_result->d_name;
            filename = os.str();

            parser->Reset();
            parser->Parse();

        } catch (csXmlParseException &e) {
            csLog::Log(csLog::Error,
                "XML parse error, %s on line: %u, column: %u, byte: 0x%02x",
                e.estring.c_str(), e.row, e.col, e.byte);
        } catch (csException &e) {
                csLog::Log(csLog::Error,
                    "%s: %s.", e.estring.c_str(), e.what());
        }
    }

    closedir(dh);

ScanPlugins_Leave:
    filename = main_conf_filename;
    free(dirent_entry);
}

csMain::csMain(int argc, char *argv[])
    : csEventClient(), log_syslog(NULL), log_logfile(NULL)
{
    bool debug = false;
    string conf_filename = _CS_MAIN_CONF;
    string log_file;
    sigset_t signal_set;

    log_stdout = new csLog();
    log_stdout->SetMask(csLog::Info | csLog::Warning | csLog::Error);

    int rc;
    static struct option options[] =
    {
        { "version", 0, 0, 'V' },
        { "config", 1, 0, 'c' },
        { "debug", 0, 0, 'd' },
        { "dump-state", 1, 0, 'D' },
        { "log", 1, 0, 'l' },
        { "help", 0, 0, 'h' },

        { NULL, 0, 0, 0 }
    };

    for (optind = 1;; ) {
        int o = 0;
        if ((rc = getopt_long(argc, argv,
            "Vc:dl:D:h?", options, &o)) == -1) break;
        switch (rc) {
        case 'V':
            Usage(true);
        case 'c':
            conf_filename = optarg;
            break;
        case 'd':
            debug = true;
            log_stdout->SetMask(
                csLog::Info | csLog::Warning | csLog::Error | csLog::Debug);
            break;
        case 'D':
            DumpStateFile(optarg);
            throw csInvalidOptionException();
        case 'l':
            log_file = optarg;
            break;
        case '?':
            csLog::Log(csLog::Info,
                "Try %s --help for more information.", argv[0]);
            throw csInvalidOptionException();
        case 'h':
            Usage();
            break;
        }
    }

    if (!debug) {
        if (daemon(1, 0) != 0)
            throw csException(errno, "daemon");
        log_syslog = new csLog("clearsyncd", LOG_PID, LOG_DAEMON);

        FILE *h_pid = fopen(_CS_PID_FILE, "w+");
        if (h_pid == NULL) {
            csLog::Log(csLog::Warning, "Error saving PID file: %s",
                _CS_PID_FILE);
        }
        else {
            if (fprintf(h_pid, "%d\n", getpid()) <= 0) {
                csLog::Log(csLog::Warning, "Error saving PID file: %s",
                    _CS_PID_FILE);
            }
            fclose(h_pid);
        }
    }

    int crypto_locks = CRYPTO_num_locks();
    if (crypto_locks > 0) {
        csCryptoMutex = new pthread_mutex_t *[crypto_locks];
        for (int i = 0; i < crypto_locks; i++) {
            csCryptoMutex[i] = new pthread_mutex_t;
            pthread_mutex_init(csCryptoMutex[i], NULL);
        }
        CRYPTO_set_locking_callback(cs_crypto_lock);
        csLog::Log(csLog::Debug,
            "Initialized %d libcrypto lock(s)", crypto_locks);
    }

    sigemptyset(&signal_set);
    csLog::Log(csLog::Debug,
        "Real-time signals: %d", SIGRTMAX - SIGRTMIN);
    for (int sigrt = SIGRTMIN; sigrt <= SIGRTMAX; sigrt++)
        sigaddset(&signal_set, sigrt);

    timer_thread = new csThreadTimer(this, signal_set);

    netlink_thread = new csThreadNetlink(this);

    csMainXmlParser *parser = new csMainXmlParser();
    conf = new csMainConf(this, conf_filename.c_str(), parser, argc, argv);
    parser->SetConf(dynamic_cast<csConf *>(conf));

    conf->Reload();
    ValidateConfiguration();

    sigfillset(&signal_set);
    sigdelset(&signal_set, SIGPROF);

    if ((rc = pthread_sigmask(SIG_BLOCK, &signal_set, NULL)) != 0)
        throw csException(rc, "pthread_sigmask");

    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGHUP);
    sigaddset(&signal_set, SIGTERM);
    sigaddset(&signal_set, SIGPIPE);
    sigaddset(&signal_set, SIGCHLD);
    sigaddset(&signal_set, SIGALRM);
    sigaddset(&signal_set, SIGUSR1);
    sigaddset(&signal_set, SIGUSR2);

    sig_handler = new csSignalHandler(this, signal_set);
    sig_handler->EventsEnable(false);
    sig_handler->Start();

    timer_thread->Start();
    netlink_thread->Start();

    map<string, csPluginLoader *>::iterator i;
    for (i = plugin.begin(); i != plugin.end(); i++) {
        try {
            i->second->GetPlugin()->Start();
        } catch (csException &e) {
            csLog::Log(csLog::Error, "Error starting plugin: %s", e.what());
        }
    }

    csLog::Log(csLog::Info, "ClearSync initialized.");
}

csMain::~csMain()
{
    map<string, csPluginLoader *>::iterator i;
    for (i = plugin.begin(); i != plugin.end(); i++) {
        delete i->second->GetPlugin();
        delete i->second;
    }

    if (sig_handler) delete sig_handler;
    if (timer_thread) delete timer_thread;
    if (netlink_thread) delete netlink_thread;
    if (conf) delete conf;
    csLog::Log(csLog::Info, "Terminated.");
    if (log_logfile) delete log_logfile;
    if (log_syslog) delete log_syslog;
    if (log_stdout) delete log_stdout;
}

void csMainConf::Reload(void)
{
    csLog::Log(csLog::Debug, "Reload configuration.");
    csConf::Reload();
    parser->Parse();
    ScanPlugins();
}

void csMain::ParseEventFilter(csPlugin *plugin, const string &text)
{
    size_t prev = 0;
    size_t next = text.find('|');
    vector<string> substr;

    while (next != string::npos) {
        string atom = text.substr(prev, next - prev);
        substr.push_back(atom);
        prev = ++next;
        next = text.find('|', prev);
    }

    if (text.length() > prev) {
        string atom = text.substr(prev);
        substr.push_back(atom);
    }

    for (vector<string>::iterator i = substr.begin();
        i != substr.end(); i++) {
        size_t length = (*i).length();
        if (length == 0) continue;
        size_t head = (*i).find_first_not_of(' ');
        size_t tail = (*i).find_last_not_of(' ');
        if (head == string::npos) head = 0;
        string atom = (*i).substr(head, tail + 1 - head);
        if (strcasecmp(atom.c_str(), plugin->GetName().c_str()) == 0) {
            csLog::Log(csLog::Warning,
                "You can not add a plugin to it's own event filter: %s",
                atom.c_str());
            continue;
        }
        plugin_event_filter[plugin].push_back(atom);
    }
}

void csMain::ValidateConfiguration(void)
{
    for (map<csPlugin *,
        vector<string> >::iterator i = plugin_event_filter.begin();
        i != plugin_event_filter.end(); i++) {
        for (vector<string>::iterator j = i->second.begin();
            j != i->second.end(); j++) {
            map<string, csPluginLoader *>::iterator p;
            p = plugin.find((*j));
            if (p != plugin.end()) continue;
            csLog::Log(csLog::Warning,
                "Event filter plugin not found: %s", (*j).c_str());
        }
    }
}

void csMain::DispatchPluginEvent(csEventPlugin *event)
{
    csPlugin *plugin = static_cast<csPlugin *>(event->GetSource());
    event->SetValue("event_source", plugin->GetName());
    for (map<csPlugin *, vector<string> >::iterator i = plugin_event_filter.begin();
        i != plugin_event_filter.end(); i++) {
        for (vector<string>::iterator j = i->second.begin();
            j != i->second.end(); j++) {
            if (strcasecmp(plugin->GetName().c_str(), (*j).c_str()))
                continue;
            map<string, csPluginLoader *>::iterator plugin_loader;
            plugin_loader = this->plugin.find(i->first->GetName());
            if (plugin_loader == this->plugin.end()) continue;
            EventDispatch(event->Clone(), plugin_loader->second->GetPlugin());
            break;
        }
    }
}

void csMain::DumpStateFile(const char *state)
{
    csPluginStateLoader state_loader;
    state_loader.DumpStateFile(state);
}

void csPluginStateLoader::DumpStateFile(const char *state)
{
    SetStateFile(state);
    LoadState();

    for (map<string, struct csPluginStateValue *>::iterator i = this->state.begin();
        i != this->state.end(); i++) {
        fprintf(stdout, "\"%s\"\n", i->first.c_str());
        csHexDump(stdout, i->second->value, i->second->length);
        fputc('\n', stdout);
    }
}

void csMain::Run(void)
{
    for ( ;; ) {
        csEvent *event = EventPopWait();

        switch (event->GetId()) {
        case csEVENT_QUIT:
            csLog::Log(csLog::Debug, "Terminating...");
            EventDestroy(event);
            return;

        case csEVENT_RELOAD:
            //conf->Reload();
            break;

        case csEVENT_PLUGIN:
            DispatchPluginEvent(static_cast<csEventPlugin *>(event));
            break;

        default:
            csLog::Log(csLog::Debug, "Unhandled event: %u", event->GetId());
            break;
        }

        EventDestroy(event);
    }
}

void csMain::Usage(bool version)
{
    csLog::Log(csLog::Info, "ClearSync v%s", _CS_VERSION);
    csLog::Log(csLog::Info, "Copyright (C) 2011-2012 ClearFoundation [%s %s]",
        __DATE__, __TIME__);
    if (version) {
        csLog::Log(csLog::Info,
            "  This program comes with ABSOLUTELY NO WARRANTY.");
        csLog::Log(csLog::Info,
            "  This is free software, and you are welcome to redistribute it");
        csLog::Log(csLog::Info,
            "  under certain conditions according to the GNU General Public");
        csLog::Log(csLog::Info,
            "  License version 3, or (at your option) any later version.");
#ifdef PACKAGE_BUGREPORT
        csLog::Log(csLog::Info, "Report bugs to: %s", PACKAGE_BUGREPORT);
#endif
    }
    else {
        csLog::Log(csLog::Info,
            "  -V, --version");
        csLog::Log(csLog::Info,
            "    Display program version and license information.");
        csLog::Log(csLog::Info,
            "  -c <file>, --config <file>");
        csLog::Log(csLog::Info,
            "    Specify an alternate configuration file.");
        csLog::Log(csLog::Info,
            "    Default: %s", _CS_MAIN_CONF);
        csLog::Log(csLog::Info,
            "  -D, --dump-state <state-file>");
        csLog::Log(csLog::Info,
            "    Dump the contents of a plugin state file.");
        csLog::Log(csLog::Info,
            "  -d, --debug");
        csLog::Log(csLog::Info,
            "    Enable debugging messages and remain in the foreground.");
    }

    throw csUsageException();
}

int main(int argc, char *argv[])
{
    csMain *cs_main = NULL;
    int rc = csEXIT_SUCCESS;

    try {
        cs_main = new csMain(argc, argv);

        cs_main->Run();

    } catch (csUsageException &e) {
    } catch (csDumpStateException &e) {
    } catch (csInvalidOptionException &e) {
        rc = csEXIT_INVALID_OPTION;
    } catch (csXmlParseException &e) {
        csLog::Log(csLog::Error,
            "XML parse error, %s on line: %u, column: %u, byte: 0x%02x",
            e.estring.c_str(), e.row, e.col, e.byte);
        rc = csEXIT_XML_PARSE_ERROR;
    } catch (csException &e) {
        csLog::Log(csLog::Error,
            "%s: %s.", e.estring.c_str(), e.what());
        rc = csEXIT_UNHANDLED_EX;
    }

    if (cs_main) delete cs_main;

    return rc;
}

// vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
