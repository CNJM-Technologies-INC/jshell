#include <iostream>
#include <string>
#include <vector>
#include <span>
#include <format>
#include <memory>
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <conio.h>
#include <algorithm>
#include <map>
#include <sstream>
#include <chrono>
#include <numeric>
#include <thread>
#include <mutex>
#include <regex>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h> // <--- THE FIX IS HERE

namespace fs = std::filesystem;

namespace jshell {

// --- Constants ---
constexpr size_t JSHELL_HISTORY_SIZE = 1000;
constexpr size_t MAX_PIPE_BUFFER = 65536;
constexpr DWORD PROCESS_TIMEOUT = 30000; // 30 seconds

// --- Core Types ---
struct Theme {
    WORD default_color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    WORD prompt_color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    WORD error_color = FOREGROUND_RED | FOREGROUND_INTENSITY;
    WORD dir_color = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    WORD help_command_color = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    WORD success_color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    WORD warning_color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
};

struct Configuration {
    std::string prompt_format = "[{cwd}] > ";
    bool enable_colors = true;
    bool auto_complete = true;
    bool save_history = true;
    size_t max_history = JSHELL_HISTORY_SIZE;
    std::string history_file = ".jshell_history";
};

struct Job {
    DWORD process_id;
    HANDLE process_handle;
    std::string command_line;
    bool is_running;
    bool is_stopped;
    int job_id;
    
    Job(DWORD pid, HANDLE handle, const std::string& cmd, int id) 
        : process_id(pid), process_handle(handle), command_line(cmd), 
          is_running(true), is_stopped(false), job_id(id) {}
};

struct ShellState {
    std::vector<std::string> history;
    size_t history_index = 0;
    std::map<std::string, std::string> aliases;
    std::map<std::string, std::string> variables;
    std::vector<std::unique_ptr<Job>> jobs;
    int next_job_id = 1;
    bool running = true;
    int last_exit_code = 0;
    Configuration config;
    fs::path shell_directory;
    
    ShellState() {
        char* appdata = nullptr;
        size_t len;
        if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata) {
            shell_directory = fs::path(appdata) / "jshell";
            free(appdata);
        } else {
            shell_directory = fs::current_path();
        }
        
        try {
            fs::create_directories(shell_directory);
        } catch (...) {
            shell_directory = fs::current_path();
        }
    }
};

struct Command {
    std::vector<std::string> args;
    std::string input_file;
    std::string output_file;
    std::string error_file;
    bool append_output = false;
    bool append_error = false;
    bool background = false;
};

struct Builtin {
    const char* name;
    int (*func)(ShellState&, std::span<const char*>);
    const char* description;
    const char* usage;
};

// --- Utility Classes ---
class ScopedHandle {
private:
    HANDLE handle_;
public:
    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) : handle_(h) {}
    ~ScopedHandle() { 
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    
    HANDLE get() const { return handle_; }
    HANDLE release() {
        HANDLE h = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return h;
    }
    
    void reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = h;
    }
    
    operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
};

class ColorGuard {
private:
    HANDLE console_;
    WORD original_attrs_;
    
public:
    ColorGuard(WORD new_attrs) : console_(GetStdHandle(STD_OUTPUT_HANDLE)) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(console_, &info)) {
            original_attrs_ = info.wAttributes;
            SetConsoleTextAttribute(console_, new_attrs);
        }
    }
    
    ~ColorGuard() {
        SetConsoleTextAttribute(console_, original_attrs_);
    }
};

// --- Forward Declarations ---
int cd(ShellState&, std::span<const char*>);
int help(ShellState&, std::span<const char*>);
int exit_shell(ShellState&, std::span<const char*>);
int pwd(ShellState&, std::span<const char*>);
int env(ShellState&, std::span<const char*>);
int set_var(ShellState&, std::span<const char*>);
int unset_var(ShellState&, std::span<const char*>);
int history(ShellState&, std::span<const char*>);
int source(ShellState&, std::span<const char*>);
int ls(ShellState&, std::span<const char*>);
int cat(ShellState&, std::span<const char*>);
int echo(ShellState&, std::span<const char*>);
int mkdir(ShellState&, std::span<const char*>);
int rm(ShellState&, std::span<const char*>);
int cls(ShellState&, std::span<const char*>);
int alias(ShellState&, std::span<const char*>);
int unalias(ShellState&, std::span<const char*>);
int touch(ShellState&, std::span<const char*>);
int cp(ShellState&, std::span<const char*>);
int mv(ShellState&, std::span<const char*>);
int grep(ShellState&, std::span<const char*>);
int find_files(ShellState&, std::span<const char*>);
int which(ShellState&, std::span<const char*>);
int ps(ShellState&, std::span<const char*>);
int kill_proc(ShellState&, std::span<const char*>);
int jobs(ShellState&, std::span<const char*>);
int fg(ShellState&, std::span<const char*>);
int bg(ShellState&, std::span<const char*>);
int code(ShellState&, std::span<const char*>);
int edit(ShellState&, std::span<const char*>);
int vi(ShellState&, std::span<const char*>);
int version(ShellState&, std::span<const char*>);

// --- Built-ins Table ---
const std::vector<Builtin> builtins = {
    {"cd",      cd,         "Change directory", "cd [directory|~|..|/]"},
    {"help",    help,       "Display help message", "help [command]"},
    {"exit",    exit_shell, "Exit the shell", "exit [code]"},
    {"pwd",     pwd,        "Print working directory", "pwd"},
    {"env",     env,        "List environment variables", "env [variable]"},
    {"set",     set_var,    "Set variable", "set <name> <value>"},
    {"unset",   unset_var,  "Unset variable", "unset <name>"},
    {"history", history,    "Show command history", "history [count]"},
    {"source",  source,     "Execute script file", "source <file>"},
    {"ls",      ls,         "List directory contents", "ls [-la] [path]"},
    {"dir",     ls,         "Alias for ls", "dir [-la] [path]"},
    {"cat",     cat,        "Display file contents", "cat <file> [files...]"},
    {"echo",    echo,       "Display text", "echo [text...]"},
    {"mkdir",   mkdir,      "Create directory", "mkdir <directory>"},
    {"rm",      rm,         "Remove files/directories", "rm [-rf] <path>"},
    {"del",     rm,         "Alias for rm", "del [-rf] <path>"},
    {"cls",     cls,        "Clear screen", "cls"},
    {"clear",   cls,        "Alias for cls", "clear"},
    {"alias",   alias,      "Create command alias", "alias [name='command']"},
    {"unalias", unalias,    "Remove alias", "unalias <name>"},
    {"touch",   touch,      "Create empty file", "touch <file>"},
    {"cp",      cp,         "Copy files", "cp <source> <destination>"},
    {"copy",    cp,         "Alias for cp", "copy <source> <destination>"},
    {"mv",      mv,         "Move/rename files", "mv <source> <destination>"},
    {"move",    mv,         "Alias for mv", "move <source> <destination>"},
    {"grep",    grep,       "Search text patterns", "grep <pattern> <file>"},
    {"find",    find_files, "Find files", "find <path> <pattern>"},
    {"which",   which,      "Locate command", "which <command>"},
    {"ps",      ps,         "List processes", "ps"},
    {"kill",    kill_proc,  "Kill process", "kill <pid>"},
    {"jobs",    jobs,       "List active jobs", "jobs"},
    {"fg",      fg,         "Bring job to foreground", "fg [job_id]"},
    {"bg",      bg,         "Send job to background", "bg [job_id]"},
    {"open",    code,       "Open applications/editors", "open [app] [path]"},
    {"edit",    edit,       "Edit file with external editor", "edit <file>"},
    {"vi",      vi,         "Vim-like built-in editor", "vi <file>"},
    {"nano",    vi,         "Alias for vi", "nano <file>"},
    {"version", version,    "Show shell version", "version"},
};

// --- Utility Functions ---
std::string get_home_directory() {
    char* path = nullptr;
    size_t len;
    if (_dupenv_s(&path, &len, "USERPROFILE") == 0 && path) {
        std::string home = path;
        free(path);
        return home;
    }
    return "";
}

std::string expand_path(const std::string& path) {
    if (path.empty()) return path;
    
    if (path[0] == '~') {
        std::string home = get_home_directory();
        if (!home.empty()) {
            return home + (path.length() > 1 ? path.substr(1) : "");
        }
    }
    
    return path;
}

std::string get_current_directory_prompt() {
    try {
        std::string home = get_home_directory();
        std::string current = fs::current_path().string();
        if (!home.empty() && current.starts_with(home)) {
            return "~" + current.substr(home.length());
        }
        return current;
    } catch (const fs::filesystem_error&) {
        return "unknown";
    }
}

std::vector<std::string> get_path_directories() {
    std::vector<std::string> paths;
    char* path_env = nullptr;
    size_t len;
    
    if (_dupenv_s(&path_env, &len, "PATH") == 0 && path_env) {
        std::string path_str = path_env;
        free(path_env);
        
        std::stringstream ss(path_str);
        std::string path;
        while (std::getline(ss, path, ';')) {
            if (!path.empty()) {
                paths.push_back(path);
            }
        }
    }
    
    return paths;
}

std::string find_executable(const std::string& name) {
    if (name.empty()) return "";
    
    // If it contains path separators, check as-is
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        if (fs::exists(name) && fs::is_regular_file(name)) {
            return name;
        }
        return "";
    }
    
    // Common executable extensions on Windows
    std::vector<std::string> extensions = {"", ".exe", ".bat", ".cmd", ".com"};
    
    // Check current directory first
    for (const auto& ext : extensions) {
        std::string full_path = name + ext;
        if (fs::exists(full_path) && fs::is_regular_file(full_path)) {
            return fs::absolute(full_path).string();
        }
    }
    
    // Check PATH directories
    auto paths = get_path_directories();
    for (const auto& dir : paths) {
        for (const auto& ext : extensions) {
            fs::path full_path = fs::path(dir) / (name + ext);
            if (fs::exists(full_path) && fs::is_regular_file(full_path)) {
                return full_path.string();
            }
        }
    }
    
    return "";
}

std::string substitute_variables(const std::string& text, const ShellState& state) {
    std::string result = text;
    
    // Replace ${VAR} and $VAR patterns
    std::regex var_pattern(R"(\$\{([^}]+)\}|\$([A-Za-z_][A-Za-z0-9_]*))");
    std::smatch match;
    
    std::string temp_result;
    auto search_start = result.cbegin();

    while (std::regex_search(search_start, result.cend(), match, var_pattern)) {
        temp_result.append(match.prefix().first, match.prefix().second);
        
        std::string var_name = match[1].str();
        if (var_name.empty()) var_name = match[2].str();
        
        std::string value;
        
        // Check shell variables first
        if (state.variables.contains(var_name)) {
            value = state.variables.at(var_name);
        } else {
            // Check environment variables
            char* env_val = nullptr;
            size_t len;
            if (_dupenv_s(&env_val, &len, var_name.c_str()) == 0 && env_val) {
                value = env_val;
                free(env_val);
            }
        }
        
        temp_result.append(value);
        search_start = match.suffix().first;
    }
    temp_result.append(search_start, result.cend());
    
    return temp_result;
}

std::vector<std::string> get_completions(const std::string& prefix, const ShellState& state) {
    std::vector<std::string> completions;
    fs::path current_path = ".";
    std::string search_prefix = prefix;

    // Handle path completion
    if (auto pos = prefix.find_last_of("/\\"); pos != std::string::npos) {
        current_path = prefix.substr(0, pos + 1);
        search_prefix = prefix.substr(pos + 1);
    }
    
    try {
        current_path = expand_path(current_path.string());
        if (fs::exists(current_path) && fs::is_directory(current_path)) {
            for (const auto& entry : fs::directory_iterator(current_path)) {
                std::string filename = entry.path().filename().string();
                if (filename.starts_with(search_prefix)) {
                    std::string completion = (current_path.string() == "." ? "" : current_path.string()) + filename;
                    if (entry.is_directory()) completion += "\\"; // Use backslash for Windows
                    completions.push_back(completion);
                }
            }
        }
    } catch (...) {
        // Ignore filesystem errors
    }

    // Add builtin commands
    for (const auto& builtin : builtins) {
        if (std::string(builtin.name).starts_with(prefix)) {
            completions.push_back(builtin.name);
        }
    }
    
    // Add aliases
    for (const auto& [name, _] : state.aliases) {
        if (name.starts_with(prefix)) {
            completions.push_back(name);
        }
    }
    
    // Add executables from PATH (only for first word)
    if (prefix.find(' ') == std::string::npos) {
        auto paths = get_path_directories();
        for (const auto& dir : paths) {
            try {
                if (fs::exists(dir) && fs::is_directory(dir)) {
                    for (const auto& entry : fs::directory_iterator(dir)) {
                        if (entry.is_regular_file()) {
                            std::string filename = entry.path().stem().string();
                            if (filename.starts_with(prefix)) {
                                completions.push_back(filename);
                            }
                        }
                    }
                }
            } catch (...) {
                // Ignore errors
            }
        }
    }
    
    // Remove duplicates and sort
    std::sort(completions.begin(), completions.end());
    completions.erase(std::unique(completions.begin(), completions.end()), completions.end());
    
    return completions;
}

std::string find_longest_common_prefix(const std::vector<std::string>& strs) {
    if (strs.empty()) return "";
    
    return std::accumulate(strs.begin() + 1, strs.end(), strs[0],
        [](const std::string& prefix, const std::string& s) {
            auto mismatch_pair = std::mismatch(prefix.begin(), prefix.end(), s.begin(), s.end());
            return std::string(prefix.begin(), mismatch_pair.first);
        });
}

void save_history(const ShellState& state) {
    if (!state.config.save_history) return;
    
    try {
        fs::path history_path = state.shell_directory / state.config.history_file;
        std::ofstream file(history_path);
        if (file) {
            for (const auto& cmd : state.history) {
                file << cmd << '\n';
            }
        }
    } catch (...) {
        // Ignore save errors
    }
}

void load_history(ShellState& state) {
    if (!state.config.save_history) return;
    
    try {
        fs::path history_path = state.shell_directory / state.config.history_file;
        if (fs::exists(history_path)) {
            std::ifstream file(history_path);
            std::string line;
            while (std::getline(file, line) && state.history.size() < state.config.max_history) {
                if (!line.empty()) {
                    state.history.push_back(line);
                }
            }
            state.history_index = state.history.size();
        }
    } catch (...) {
        // Ignore load errors
    }
}

void redraw_line(const std::string& prompt, const std::string& line) {
    std::cout << "\r" << std::string(120, ' ') << "\r";
    
    const Theme theme;
    ColorGuard guard(theme.prompt_color);
    std::cout << prompt;
    
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
    std::cout << line;
    std::cout.flush();
}

std::string read_line(ShellState& state) {
    std::string prompt_template = state.config.prompt_format;
    std::string cwd = get_current_directory_prompt();
    
    // Simple variable substitution for prompt
    size_t pos = prompt_template.find("{cwd}");
    if (pos != std::string::npos) {
        prompt_template.replace(pos, 5, cwd);
    }
    
    std::string line;
    size_t cursor_pos = 0;
    int last_char = 0;
    
    auto redraw_with_cursor = [&]() {
        // Move to beginning of line
        std::cout << "\r";
        
        // Draw prompt
        const Theme theme;
        ColorGuard guard(theme.prompt_color);
        std::cout << prompt_template;
        
        // Draw line content
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
        std::cout << line;
        
        // Add a few spaces to clear any leftover characters, then backspace
        std::cout << "  \b\b";
        
        // Position cursor correctly
        if (cursor_pos < line.length()) {
            // Move cursor back to correct position
            for (size_t i = cursor_pos; i < line.length(); ++i) {
                std::cout << '\b';
            }
        }
        std::cout.flush();
    };
    
    redraw_with_cursor();

    while (true) {
        int ch = _getch();
        
        if (ch == 13) { // Enter
            std::cout << "\n";
            break;
        } else if (ch == 224 || ch == 0) { // Extended keys (handle both 224 and 0)
            ch = _getch();
            if (ch == 72 && !state.history.empty()) { // Up arrow
                if (state.history_index > 0) {
                    state.history_index--;
                    line = state.history[state.history_index];
                    cursor_pos = line.length();
                    redraw_with_cursor();
                }
            } else if (ch == 80) { // Down arrow
                if (state.history_index < state.history.size()) {
                    state.history_index++;
                    line = (state.history_index < state.history.size()) ? 
                           state.history[state.history_index] : "";
                    cursor_pos = line.length();
                    redraw_with_cursor();
                }
            } else if (ch == 75) { // Left arrow
                if (cursor_pos > 0) {
                    cursor_pos--;
                    std::cout << '\b';
                    std::cout.flush();
                }
            } else if (ch == 77) { // Right arrow
                if (cursor_pos < line.length()) {
                    std::cout << line[cursor_pos];
                    cursor_pos++;
                    std::cout.flush();
                }
            } else if (ch == 71) { // Home
                while (cursor_pos > 0) {
                    cursor_pos--;
                    std::cout << '\b';
                }
                std::cout.flush();
            } else if (ch == 79) { // End
                while (cursor_pos < line.length()) {
                    std::cout << line[cursor_pos];
                    cursor_pos++;
                }
                std::cout.flush();
            }
            // Ignore any other extended keys to prevent weird characters
            continue;
        } else if (ch == 9 && state.config.auto_complete) { // Tab
            std::string prefix = line.substr(0, cursor_pos);
            auto completions = get_completions(prefix, state);
            if (completions.empty()) continue;

            if (completions.size() == 1) {
                line = completions[0] + line.substr(cursor_pos);
                cursor_pos = completions[0].length();
            } else {
                std::string lcp = find_longest_common_prefix(completions);
                if (!lcp.empty() && lcp.length() > prefix.length()) {
                    line = lcp + line.substr(cursor_pos);
                    cursor_pos = lcp.length();
                } else if (last_char == 9) { // Double tab
                    std::cout << "\n";
                    for (size_t i = 0; i < completions.size(); ++i) {
                        std::cout << std::format("{:<20}", completions[i]);
                        if ((i + 1) % 4 == 0) std::cout << "\n";
                    }
                    if (completions.size() % 4 != 0) std::cout << "\n";
                }
            }
            redraw_with_cursor();
        } else if (ch == 8) { // Backspace
            if (cursor_pos > 0) {
                line.erase(cursor_pos - 1, 1);
                cursor_pos--;
                redraw_with_cursor();
            }
        } else if (ch == 127) { // Delete
            if (cursor_pos < line.length()) {
                line.erase(cursor_pos, 1);
                redraw_with_cursor();
            }
        } else if (ch == 3) { // Ctrl+C
            std::cout << "^C\n";
            line.clear();
            cursor_pos = 0;
            redraw_with_cursor();
        } else if (isprint(ch)) { // Printable characters
            char c = static_cast<char>(ch);
            
            if (cursor_pos == line.length()) {
                // Simple case: adding to end of line
                line += c;
                cursor_pos++;
                std::cout << c;
                std::cout.flush();
            } else {
                // Complex case: inserting in middle
                line.insert(cursor_pos, 1, c);
                cursor_pos++;
                redraw_with_cursor();
            }
        }
        
        last_char = ch;
    }

    // Add to history
    if (!line.empty() && (state.history.empty() || state.history.back() != line)) {
        if (state.history.size() >= state.config.max_history) {
            state.history.erase(state.history.begin());
        }
        state.history.push_back(line);
    }
    state.history_index = state.history.size();
    
    return line;
}

std::vector<std::string> tokenize(std::string_view str) {
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;
    char quote_char = '\0';
    
    for (char c : str) {
        if ((c == '"' || c == '\'') && !in_quotes) {
            in_quotes = true;
            quote_char = c;
        } else if (c == quote_char && in_quotes) {
            in_quotes = false;
            quote_char = '\0';
        } else if (std::isspace(c) && !in_quotes) {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } else {
            current_token += c;
        }
    }
    
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }
    
    return tokens;
}

Command parse_command(std::string_view command_str, const ShellState& state) {
    Command cmd;
    std::string temp_str = substitute_variables(std::string(command_str), state);
    
    // Check for background execution
    if (temp_str.ends_with('&')) {
        cmd.background = true;
        temp_str.pop_back(); // Remove the '&'
        // Trim trailing whitespace
        size_t last = temp_str.find_last_not_of(" \t");
        if(std::string::npos != last) temp_str = temp_str.substr(0, last + 1);
    }
    
    // Parse stderr redirection (2>> and 2>)
    size_t pos_err_append = temp_str.find("2>>");
    size_t pos_err = (pos_err_append != std::string::npos) ? std::string::npos : temp_str.find("2>");
    
    if (pos_err_append != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_err_append + 3));
        if (!tokens.empty()) {
            cmd.error_file = tokens[0];
            cmd.append_error = true;
        }
        temp_str = temp_str.substr(0, pos_err_append);
    } else if (pos_err != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_err + 2));
        if (!tokens.empty()) {
            cmd.error_file = tokens[0];
        }
        temp_str = temp_str.substr(0, pos_err);
    }
    
    // Parse output redirection (>> and >)
    size_t pos_append = temp_str.find(">>");
    size_t pos_out = (pos_append != std::string::npos) ? std::string::npos : temp_str.find('>');
    
    if (pos_append != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_append + 2));
        if (!tokens.empty()) {
            cmd.output_file = tokens[0];
            cmd.append_output = true;
        }
        temp_str = temp_str.substr(0, pos_append);
    } else if (pos_out != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_out + 1));
        if (!tokens.empty()) {
            cmd.output_file = tokens[0];
        }
        temp_str = temp_str.substr(0, pos_out);
    }
    
    // Parse input redirection (<)
    size_t pos_in = temp_str.find('<');
    if (pos_in != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_in + 1));
        if (!tokens.empty()) {
            cmd.input_file = tokens[0];
        }
        temp_str = temp_str.substr(0, pos_in);
    }
    
    cmd.args = tokenize(temp_str);
    
    // Expand paths in arguments
    for (auto& arg : cmd.args) {
        arg = expand_path(arg);
    }
    
    return cmd;
}

std::vector<Command> parse_pipeline(const std::string& line, const ShellState& state) {
    if (line.empty()) return {};
    
    std::vector<Command> commands;
    std::stringstream ss(line);
    std::string segment;
    
    while (std::getline(ss, segment, '|')) {
        commands.push_back(parse_command(segment, state));
    }
    
    return commands;
}

struct ParsedArgs {
    std::map<char, bool> flags;
    std::map<std::string, std::string> long_flags;
    std::vector<std::string> non_flag_args;
};

ParsedArgs parse_args(std::span<const char*> args) {
    ParsedArgs result;
    
    for (size_t i = 1; i < args.size(); ++i) {
        std::string arg = args[i];
        
        if (arg.starts_with("--")) {
            // Long flag
            auto eq_pos = arg.find('=');
            if (eq_pos != std::string::npos) {
                std::string key = arg.substr(2, eq_pos - 2);
                std::string value = arg.substr(eq_pos + 1);
                result.long_flags[key] = value;
            } else {
                result.long_flags[arg.substr(2)] = "";
            }
        } else if (arg.starts_with('-') && arg.length() > 1) {
            // Short flags
            for (size_t j = 1; j < arg.length(); ++j) {
                result.flags[arg[j]] = true;
            }
        } else {
            result.non_flag_args.push_back(arg);
        }
    }
    
    return result;
}

int launch_process(Command& cmd, HANDLE hInput, HANDLE hOutput, HANDLE hError, ShellState* state = nullptr, bool wait = true) {
    if (cmd.args.empty()) return 1;

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = hInput != INVALID_HANDLE_VALUE ? hInput : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOutput != INVALID_HANDLE_VALUE ? hOutput : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = hError != INVALID_HANDLE_VALUE ? hError : GetStdHandle(STD_ERROR_HANDLE);

    ScopedHandle input_file, output_file, error_file;
    
    // Handle input redirection
    if (!cmd.input_file.empty()) {
        input_file.reset(CreateFileA(
            cmd.input_file.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        
        if (!input_file) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: Cannot open input file '{}': {}\n", 
                                    cmd.input_file, std::system_category().message(GetLastError()));
            return 1;
        }
        si.hStdInput = input_file.get();
    }
    
    // Handle output redirection
    if (!cmd.output_file.empty()) {
        DWORD creation = cmd.append_output ? OPEN_ALWAYS : CREATE_ALWAYS;
        output_file.reset(CreateFileA(
            cmd.output_file.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            creation,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        
        if (!output_file) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: Cannot open output file '{}': {}\n",
                                    cmd.output_file, std::system_category().message(GetLastError()));
            return 1;
        }
        
        if (cmd.append_output) {
            SetFilePointer(output_file.get(), 0, nullptr, FILE_END);
        }
        
        si.hStdOutput = output_file.get();
    }
    
    // Handle stderr redirection
    if (!cmd.error_file.empty()) {
        DWORD creation = cmd.append_error ? OPEN_ALWAYS : CREATE_ALWAYS;
        error_file.reset(CreateFileA(
            cmd.error_file.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            creation,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));
        
        if (!error_file) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: Cannot open error file '{}': {}\n",
                                    cmd.error_file, std::system_category().message(GetLastError()));
            return 1;
        }
        
        if (cmd.append_error) {
            SetFilePointer(error_file.get(), 0, nullptr, FILE_END);
        }
        
        si.hStdError = error_file.get();
    }

    // Find executable
    std::string executable = find_executable(cmd.args[0]);
    if (executable.empty()) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: Command not found: '{}'\n", cmd.args[0]);
        return 127;
    }

    // Build command line
    std::string cmd_line;
    for (size_t i = 1; i < cmd.args.size(); ++i) { // Start from 1 as executable is handled separately
        if (cmd.args[i].find(' ') != std::string::npos) {
            cmd_line += "\"" + cmd.args[i] + "\" ";
        } else {
            cmd_line += cmd.args[i] + " ";
        }
    }
    if(!cmd_line.empty()) cmd_line.pop_back();

    DWORD creation_flags = 0;
    if (cmd.background) {
        creation_flags = DETACHED_PROCESS;
    }

    if (!CreateProcessA(
        executable.c_str(),
        cmd_line.empty() ? nullptr : cmd_line.data(), // Pass nullptr if no args
        nullptr,
        nullptr,
        TRUE,
        creation_flags,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: Failed to execute '{}': {}\n",
                                cmd.args[0], std::system_category().message(GetLastError()));
        return 1;
    }
    
    ScopedHandle hThread(pi.hThread);
    DWORD exit_code = 0;
    
    if (wait && !cmd.background) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
    } else if (cmd.background && state) {
        // Add to job list
        std::string cmd_str;
        for (const auto& arg : cmd.args) {
            cmd_str += arg + " ";
        }
        if (!cmd_str.empty()) cmd_str.pop_back();
        
        auto job = std::make_unique<Job>(pi.dwProcessId, pi.hProcess, cmd_str, state->next_job_id++);
        std::cout << std::format("[{}] {} {}\n", job->job_id, pi.dwProcessId, cmd_str);
        state->jobs.push_back(std::move(job));
    } else {
        CloseHandle(pi.hProcess);
    }
    
    return static_cast<int>(exit_code);
}

// Forward declare execute function
int execute(ShellState& state, std::vector<Command>& commands);

// --- Built-in Command Implementations ---
// (All implementations from before are here, they are correct)
int cd(ShellState&, std::span<const char*> args) {
    std::string target_dir;
    
    if (args.size() < 2) {
        target_dir = get_home_directory();
    } else {
        std::string arg = args[1];
        if (arg == "~") {
            target_dir = get_home_directory();
        } else if (arg == "-") {
            // TODO: Implement previous directory tracking
            target_dir = get_home_directory();
        } else {
            target_dir = expand_path(arg);
        }
    }
    
    if (target_dir.empty()) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: HOME directory not found\n";
        return 1;
    }
    
    try {
        fs::current_path(target_dir);
        return 0;
    } catch (const fs::filesystem_error& e) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: cd: {}\n", e.what());
        return 1;
    }
}

int help(ShellState&, std::span<const char*> args) {
    const Theme theme;
    
    if (args.size() > 1) {
        // Show help for specific command
        std::string cmd_name = args[1];
        auto it = std::find_if(builtins.begin(), builtins.end(),
                              [&](const Builtin& b) { return b.name == cmd_name; });
        
        if (it != builtins.end()) {
            ColorGuard header_guard(theme.prompt_color);
            std::cout << std::format("{} - {}\n", it->name, it->description);
            
            SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
            std::cout << std::format("Usage: {}\n", it->usage);
        } else {
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: No help available for '{}'\n", cmd_name);
            return 1;
        }
    } else {
        // Show general help
        ColorGuard header_guard(theme.prompt_color);
        std::cout << "jshell - Enhanced C++ Shell for Windows v2.0\n\n";
        
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
        std::cout << "Built-in commands:\n";

        for (const auto& builtin : builtins) {
            ColorGuard cmd_guard(theme.help_command_color);
            std::cout << std::format("  {:12}", builtin.name);
            
            SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
            std::cout << std::format(" - {}\n", builtin.description);
        }
        
        std::cout << "\nUse 'help <command>' for detailed usage information.\n";
    }
    
    return 0;
}

int exit_shell(ShellState& state, std::span<const char*> args) {
    int exit_code = 0;
    if (args.size() > 1) {
        try {
            exit_code = std::stoi(args[1]);
        } catch (...) {
            exit_code = 1;
        }
    }
    
    save_history(state);
    state.running = false;
    state.last_exit_code = exit_code;
    return exit_code;
}

int pwd(ShellState&, std::span<const char*>) {
    try {
        std::cout << fs::current_path().string() << '\n';
        return 0;
    } catch (const fs::filesystem_error& e) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: pwd: {}\n", e.what());
        return 1;
    }
}

int env(ShellState& state, std::span<const char*> args) {
    if (args.size() > 1) {
        // Show specific variable
        std::string var_name = args[1];
        
        if (state.variables.contains(var_name)) {
            std::cout << std::format("{}={}\n", var_name, state.variables[var_name]);
        } else {
            char* env_val = nullptr;
            size_t len;
            if (_dupenv_s(&env_val, &len, var_name.c_str()) == 0 && env_val) {
                std::cout << std::format("{}={}\n", var_name, env_val);
                free(env_val);
            } else {
                const Theme theme;
                ColorGuard guard(theme.error_color);
                std::cerr << std::format("jshell: Variable '{}' not found\n", var_name);
                return 1;
            }
        }
    } else {
        // Show all environment variables
        for (char** env_var = _environ; *env_var; ++env_var) {
            std::cout << *env_var << '\n';
        }
        
        // Show shell variables
        if (!state.variables.empty()) {
            const Theme theme;
            ColorGuard guard(theme.help_command_color);
            std::cout << "\nShell variables:\n";
            
            SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
            for (const auto& [name, value] : state.variables) {
                std::cout << std::format("{}={}\n", name, value);
            }
        }
    }
    
    return 0;
}

int set_var(ShellState& state, std::span<const char*> args) {
    if (args.size() < 3) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: set <NAME> <VALUE>\n";
        return 1;
    }
    
    std::string name = args[1];
    std::string value = args[2];
    
    // Join remaining arguments as value
    for (size_t i = 3; i < args.size(); ++i) {
        value += " " + std::string(args[i]);
    }
    
    // Set both shell variable and environment variable
    state.variables[name] = value;
    if (!SetEnvironmentVariableA(name.c_str(), value.c_str())) {
        const Theme theme;
        ColorGuard guard(theme.warning_color);
        std::cerr << "jshell: Warning: Failed to set environment variable\n";
    }
    
    return 0;
}

int unset_var(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: unset <NAME>\n";
        return 1;
    }
    
    std::string name = args[1];
    state.variables.erase(name);
    
    if (!SetEnvironmentVariableA(name.c_str(), nullptr)) {
        const Theme theme;
        ColorGuard guard(theme.warning_color);
        std::cerr << "jshell: Warning: Failed to unset environment variable\n";
    }
    
    return 0;
}

int history(ShellState& state, std::span<const char*> args) {
    size_t count = state.history.size();
    
    if (args.size() > 1) {
        try {
            count = std::min(count, static_cast<size_t>(std::stoi(args[1])));
        } catch (...) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << "jshell: Invalid number\n";
            return 1;
        }
    }
    
    size_t start = state.history.size() > count ? state.history.size() - count : 0;
    
    for (size_t i = start; i < state.history.size(); ++i) {
        std::cout << std::format("{:5}: {}\n", i + 1, state.history[i]);
    }
    
    return 0;
}

int source(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: source <script_file>\n";
        return 1;
    }
    
    std::string filepath = expand_path(args[1]);
    std::ifstream file(filepath);
    
    if (!file) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: Failed to open script '{}'\n", filepath);
        return 1;
    }
    
    std::string line;
    int line_number = 0;
    
    while (std::getline(file, line)) {
        line_number++;
        
        // Skip empty lines and comments
        if (line.empty() || line.starts_with('#')) continue;
        
        try {
            auto commands = parse_pipeline(line, state);
            if (!commands.empty()) {
                execute(state, commands);
            }
        } catch (const std::exception& e) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: Error at line {}: {}\n", line_number, e.what());
        }
        
        if (!state.running) break;
    }
    
    return 0;
}

int ls(ShellState&, std::span<const char*> args) {
    ParsedArgs parsed = parse_args(args);
    bool long_format = parsed.flags['l'];
    bool show_all = parsed.flags['a'];
    
    fs::path path = parsed.non_flag_args.empty() ? "." : expand_path(parsed.non_flag_args[0]);
    const Theme theme;

    try {
        if (!fs::exists(path)) {
            throw fs::filesystem_error("Path does not exist", path, 
                                     std::make_error_code(std::errc::no_such_file_or_directory));
        }
        
        if (fs::is_regular_file(path)) {
            auto ftime = fs::last_write_time(path);
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
            
            char time_buf[20];
            std::strftime(time_buf, sizeof(time_buf), "%b %d %H:%M", std::localtime(&cftime));
            
            if (long_format) {
                uintmax_t size = fs::file_size(path);
                std::cout << std::format("-rwx------ {:>10} {} {}\n", 
                                       size, time_buf, path.filename().string());
            } else {
                std::cout << path.filename().string() << '\n';
            }
            return 0;
        }
        
        if (!fs::is_directory(path)) {
            throw fs::filesystem_error("Not a directory", path,
                                     std::make_error_code(std::errc::not_a_directory));
        }

        for (const auto& entry : fs::directory_iterator(path)) {
            std::string filename = entry.path().filename().string();
            
            if (!show_all && filename.starts_with('.')) continue;

            if (long_format) {
                auto ftime = entry.last_write_time();
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
                
                char time_buf[20];
                std::strftime(time_buf, sizeof(time_buf), "%b %d %H:%M", std::localtime(&cftime));
                
                std::cout << (entry.is_directory() ? 'd' : '-');
                std::cout << "rwx------ ";
                
                uintmax_t size = 0;
                try {
                    size = entry.is_directory() ? 0 : fs::file_size(entry);
                } catch (...) {
                    size = 0;
                }
                
                std::cout << std::format("{:>10} {} ", size, time_buf);
            }
            
            if (entry.is_directory()) {
                ColorGuard guard(theme.dir_color);
                std::cout << filename;
            } else {
                std::cout << filename;
            }
            
            if (entry.is_directory()) std::cout << '/';
            std::cout << '\n';
        }
        
    } catch (const fs::filesystem_error& e) {
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: ls: {}\n", e.what());
        return 1;
    }
    
    return 0;
}

int cat(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: cat <file> [files...]\n";
        return 1;
    }
    
    int exit_code = 0;
    
    for (size_t i = 1; i < args.size(); ++i) {
        std::string filepath = expand_path(args[i]);
        std::ifstream file(filepath, std::ios::binary);
        
        if (!file) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: cat: Cannot open file '{}'\n", filepath);
            exit_code = 1;
            continue;
        }
        
        std::cout << file.rdbuf();
    }
    
    return exit_code;
}

int echo(ShellState&, std::span<const char*> args) {
    bool no_newline = false;
    size_t start_idx = 1;
    
    // Check for -n flag
    if (args.size() > 1 && std::string(args[1]) == "-n") {
        no_newline = true;
        start_idx = 2;
    }
    
    for (size_t i = start_idx; i < args.size(); ++i) {
        std::cout << args[i];
        if (i < args.size() - 1) std::cout << ' ';
    }
    
    if (!no_newline) std::cout << '\n';
    
    return 0;
}

int mkdir(ShellState&, std::span<const char*> args) {
    ParsedArgs parsed = parse_args(args);
    bool parents = parsed.flags['p'];
    
    if (parsed.non_flag_args.empty()) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: mkdir [-p] <directory>\n";
        return 1;
    }
    
    int exit_code = 0;
    
    for (const auto& dir_name : parsed.non_flag_args) {
        std::string dir_path = expand_path(dir_name);
        
        try {
            if (parents) {
                fs::create_directories(dir_path);
            } else {
                fs::create_directory(dir_path);
            }
        } catch (const fs::filesystem_error& e) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: mkdir: {}\n", e.what());
            exit_code = 1;
        }
    }
    
    return exit_code;
}

int rm(ShellState&, std::span<const char*> args) {
    ParsedArgs parsed = parse_args(args);
    bool recursive = parsed.flags['r'];
    bool force = parsed.flags['f'];
    
    if (parsed.non_flag_args.empty()) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: rm [-rf] <path>\n";
        return 1;
    }
    
    int exit_code = 0;
    
    for (const auto& path_name : parsed.non_flag_args) {
        std::string path_str = expand_path(path_name);
        
        try {
            if (!fs::exists(path_str)) {
                if (!force) {
                    const Theme theme;
                    ColorGuard guard(theme.error_color);
                    std::cerr << std::format("jshell: rm: '{}' does not exist\n", path_str);
                    exit_code = 1;
                }
                continue;
            }
            
            if (fs::is_directory(path_str) && !recursive) {
                const Theme theme;
                ColorGuard guard(theme.error_color);
                std::cerr << std::format("jshell: rm: '{}' is a directory (use -r for recursive removal)\n", path_str);
                exit_code = 1;
                continue;
            }
            
            if (recursive) {
                fs::remove_all(path_str);
            } else {
                fs::remove(path_str);
            }
            
        } catch (const fs::filesystem_error& e) {
            if (!force) {
                const Theme theme;
                ColorGuard guard(theme.error_color);
                std::cerr << std::format("jshell: rm: {}\n", e.what());
                exit_code = 1;
            }
        }
    }
    
    return exit_code;
}

int cls(ShellState&, std::span<const char*>) {
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD written;
    
    if (GetConsoleScreenBufferInfo(console, &info)) {
        DWORD size = info.dwSize.X * info.dwSize.Y;
        COORD coord = {0, 0};
        
        FillConsoleOutputCharacterA(console, ' ', size, coord, &written);
        FillConsoleOutputAttribute(console, info.wAttributes, size, coord, &written);
        SetConsoleCursorPosition(console, coord);
    }
    
    return 0;
}

int alias(ShellState& state, std::span<const char*> args) {
    if (args.size() == 1) {
        // Show all aliases
        if (state.aliases.empty()) {
            std::cout << "No aliases defined.\n";
        } else {
            for (const auto& [name, command] : state.aliases) {
                std::cout << std::format("{}='{}'\n", name, command);
            }
        }
        return 0;
    }
    
    // Join all arguments after 'alias'
    std::string arg_str;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) arg_str += ' ';
        arg_str += args[i];
    }
    
    auto eq_pos = arg_str.find('=');
    if (eq_pos == std::string::npos) {
        // Show specific alias
        if (state.aliases.contains(arg_str)) {
            std::cout << std::format("{}='{}'\n", arg_str, state.aliases[arg_str]);
        } else {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: alias '{}' not found\n", arg_str);
            return 1;
        }
    } else {
        // Set alias
        std::string name = arg_str.substr(0, eq_pos);
        std::string command = arg_str.substr(eq_pos + 1);
        
        // Remove quotes if present
        if ((command.starts_with('\'') && command.ends_with('\'')) ||
            (command.starts_with('"') && command.ends_with('"'))) {
            command = command.substr(1, command.length() - 2);
        }
        
        state.aliases[name] = command;
    }
    
    return 0;
}

int unalias(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: unalias <name>\n";
        return 1;
    }
    
    std::string name = args[1];
    if (state.aliases.erase(name) == 0) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: alias '{}' not found\n", name);
        return 1;
    }
    
    return 0;
}

int touch(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: touch <filename>\n";
        return 1;
    }
    
    int exit_code = 0;
    
    for (size_t i = 1; i < args.size(); ++i) {
        std::string filepath = expand_path(args[i]);
        
        try {
            if (fs::exists(filepath)) {
                // Update timestamp
                fs::last_write_time(filepath, fs::file_time_type::clock::now());
            } else {
                // Create file
                std::ofstream file(filepath);
                if (!file) {
                    const Theme theme;
                    ColorGuard guard(theme.error_color);
                    std::cerr << std::format("jshell: touch: Cannot create file '{}'\n", filepath);
                    exit_code = 1;
                }
            }
        } catch (const fs::filesystem_error& e) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: touch: {}\n", e.what());
            exit_code = 1;
        }
    }
    
    return exit_code;
}

int cp(ShellState&, std::span<const char*> args) {
    ParsedArgs parsed = parse_args(args);
    bool recursive = parsed.flags['r'];
    
    if (parsed.non_flag_args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: cp [-r] <source> <destination>\n";
        return 1;
    }
    
    std::string src = expand_path(parsed.non_flag_args[0]);
    std::string dst = expand_path(parsed.non_flag_args[1]);
    
    try {
        if (!fs::exists(src)) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: cp: Source '{}' does not exist\n", src);
            return 1;
        }
        
        if (fs::is_directory(src)) {
            if (!recursive) {
                const Theme theme;
                ColorGuard guard(theme.error_color);
                std::cerr << "jshell: cp: Source is a directory (use -r for recursive copy)\n";
                return 1;
            }
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        } else {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        }
        
        return 0;
        
    } catch (const fs::filesystem_error& e) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: cp: {}\n", e.what());
        return 1;
    }
}

int mv(ShellState&, std::span<const char*> args) {
    if (args.size() < 3) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: mv <source> <destination>\n";
        return 1;
    }
    
    std::string src = expand_path(args[1]);
    std::string dst = expand_path(args[2]);
    
    try {
        fs::rename(src, dst);
        return 0;
    } catch (const fs::filesystem_error& e) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: mv: {}\n", e.what());
        return 1;
    }
}

int grep(ShellState&, std::span<const char*> args) {
    if (args.size() < 3) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: grep <pattern> <file>\n";
        return 1;
    }
    
    std::string pattern = args[1];
    std::string filepath = expand_path(args[2]);
    
    std::ifstream file(filepath);
    if (!file) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: grep: Cannot open file '{}'\n", filepath);
        return 1;
    }
    
    std::string line;
    size_t line_number = 1;
    bool found = false;
    
    try {
        std::regex regex_pattern(pattern, std::regex::icase);
        
        while (std::getline(file, line)) {
            if (std::regex_search(line, regex_pattern)) {
                std::cout << std::format("{}:{}: {}\n", filepath, line_number, line);
                found = true;
            }
            line_number++;
        }
    } catch (const std::regex_error&) {
        // Fall back to simple string search
        file.clear();
        file.seekg(0);
        line_number = 1;
        
        while (std::getline(file, line)) {
            if (line.find(pattern) != std::string::npos) {
                std::cout << std::format("{}:{}: {}\n", filepath, line_number, line);
                found = true;
            }
            line_number++;
        }
    }
    
    return found ? 0 : 1;
}

int find_files(ShellState&, std::span<const char*> args) {
    if (args.size() < 3) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: find <path> <pattern>\n";
        return 1;
    }
    
    std::string search_path = expand_path(args[1]);
    std::string pattern = args[2];
    
    try {
        std::regex regex_pattern(pattern, std::regex::icase);
        bool found = false;
        
        for (const auto& entry : fs::recursive_directory_iterator(search_path, fs::directory_options::skip_permission_denied)) {
            try {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (std::regex_search(filename, regex_pattern)) {
                        std::cout << entry.path().string() << '\n';
                        found = true;
                    }
                }
            } catch(const fs::filesystem_error&) { continue; }
        }
        
        return found ? 0 : 1;
        
    } catch (const std::regex_error&) {
        // Fall back to simple string search
        bool found = false;
        
        try {
            for (const auto& entry : fs::recursive_directory_iterator(search_path, fs::directory_options::skip_permission_denied)) {
                try {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        if (filename.find(pattern) != std::string::npos) {
                            std::cout << entry.path().string() << '\n';
                            found = true;
                        }
                    }
                } catch(const fs::filesystem_error&) { continue; }
            }
        } catch (const fs::filesystem_error& e) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: find: {}\n", e.what());
            return 1;
        }
        
        return found ? 0 : 1;
    } catch (const fs::filesystem_error& e) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: find: {}\n", e.what());
        return 1;
    }
}

int which(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: which <command>\n";
        return 1;
    }
    
    std::string cmd_name = args[1];
    
    // Check aliases
    if (state.aliases.contains(cmd_name)) {
        std::cout << std::format("{}: aliased to '{}'\n", cmd_name, state.aliases[cmd_name]);
        return 0;
    }
    
    // Check builtins
    for (const auto& builtin : builtins) {
        if (builtin.name == cmd_name) {
            std::cout << std::format("{}: shell builtin\n", cmd_name);
            return 0;
        }
    }
    
    // Find executable
    std::string path = find_executable(cmd_name);
    if (!path.empty()) {
        std::cout << path << '\n';
        return 0;
    }
    
    const Theme theme;
    ColorGuard guard(theme.error_color);
    std::cerr << std::format("jshell: which: '{}' not found\n", cmd_name);
    return 1;
}

int ps(ShellState&, std::span<const char*>) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: ps: Cannot create process snapshot\n";
        return 1;
    }
    
    PROCESSENTRY32 pe = { sizeof(pe) };
    
    std::cout << std::format("{:>8} {:>8} {}\n", "PID", "PPID", "NAME");
    std::cout << std::string(40, '-') << '\n';
    
    if (Process32First(snapshot.get(), &pe)) {
        do {
            std::cout << std::format("{:>8} {:>8} {}\n", 
                                   pe.th32ProcessID, 
                                   pe.th32ParentProcessID, 
                                   pe.szExeFile);
        } while (Process32Next(snapshot.get(), &pe));
    }
    
    return 0;
}

int kill_proc(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: kill <pid>\n";
        return 1;
    }
    
    DWORD pid;
    try {
        pid = std::stoul(args[1]);
    } catch (...) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: kill: Invalid process ID\n";
        return 1;
    }
    
    ScopedHandle process(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
    if (!process) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: kill: Cannot open process {}: {}\n", pid, std::system_category().message(GetLastError()));
        return 1;
    }
    
    if (!TerminateProcess(process.get(), 1)) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: kill: Cannot terminate process {}: {}\n", pid, std::system_category().message(GetLastError()));
        return 1;
    }
    
    const Theme theme;
    ColorGuard guard(theme.success_color);
    std::cout << std::format("Process {} terminated\n", pid);
    return 0;
}

int jobs(ShellState& state, std::span<const char*>) {
    if (state.jobs.empty()) {
        std::cout << "No active jobs.\n";
        return 0;
    }
    
    // Clean up finished jobs first
    for (auto it = state.jobs.begin(); it != state.jobs.end();) {
        DWORD exit_code;
        if (GetExitCodeProcess((*it)->process_handle, &exit_code) && exit_code != STILL_ACTIVE) {
            const Theme theme;
            ColorGuard guard(theme.success_color);
            std::cout << std::format("[{}]+ Done                    {}\n", (*it)->job_id, (*it)->command_line);
            CloseHandle((*it)->process_handle);
            it = state.jobs.erase(it);
        } else {
            ++it;
        }
    }
    
    // Show remaining jobs
    for (const auto& job : state.jobs) {
        std::string status = job->is_stopped ? "Stopped" : "Running";
        std::cout << std::format("[{}]  {} {:>8}     {}\n", 
                                job->job_id, status, job->process_id, job->command_line);
    }
    
    return 0;
}

int fg(ShellState& state, std::span<const char*> args) {
    if (state.jobs.empty()) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: fg: no current job\n";
        return 1;
    }
    
    std::unique_ptr<Job> job;
    
    if (args.size() > 1) {
        // Find job by ID
        int job_id = std::stoi(args[1]);
        auto it = std::find_if(state.jobs.begin(), state.jobs.end(),
                              [job_id](const auto& j) { return j->job_id == job_id; });
        
        if (it == state.jobs.end()) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: fg: job {} not found\n", job_id);
            return 1;
        }
        
        job = std::move(*it);
        state.jobs.erase(it);
    } else {
        // Use most recent job
        job = std::move(state.jobs.back());
        state.jobs.pop_back();
    }
    
    std::cout << job->command_line << "\n";
    
    // Wait for the job to complete
    DWORD exit_code;
    WaitForSingleObject(job->process_handle, INFINITE);
    GetExitCodeProcess(job->process_handle, &exit_code);
    CloseHandle(job->process_handle);
    
    return static_cast<int>(exit_code);
}

int bg(ShellState& state, std::span<const char*> args) {
    if (state.jobs.empty()) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: bg: no current job\n";
        return 1;
    }
    
    Job* target_job = nullptr;
    
    if (args.size() > 1) {
        // Find job by ID
        int job_id = std::stoi(args[1]);
        auto it = std::find_if(state.jobs.begin(), state.jobs.end(),
                              [job_id](const auto& j) { return j->job_id == job_id; });
        
        if (it == state.jobs.end()) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << std::format("jshell: bg: job {} not found\n", job_id);
            return 1;
        }
        
        target_job = it->get();
    } else {
        // Use most recent job
        target_job = state.jobs.back().get();
    }
    
    if (!target_job->is_stopped) {
        const Theme theme;
        ColorGuard guard(theme.warning_color);
        std::cerr << std::format("jshell: bg: job {} is already running\n", target_job->job_id);
        return 1;
    }
    
    target_job->is_stopped = false;
    target_job->is_running = true;
    
    std::cout << std::format("[{}]+ {} &\n", target_job->job_id, target_job->command_line);
    
    return 0;
}

int code(ShellState&, std::span<const char*> args) {
    const Theme theme;
    
    // Application shortcuts and their common executable names
    std::map<std::string, std::vector<std::string>> app_shortcuts = {
        // Editors
        {"vscode", {"code", "code-insiders"}},
        {"vs", {"code", "code-insiders"}},
        {"code", {"code"}},
        {"code-insiders", {"code-insiders"}},
        {"kiro", {"kiro", "Kiro"}},
        {"notepad++", {"notepad++", "notepad++.exe"}},
        {"npp", {"notepad++", "notepad++.exe"}},
        {"sublime", {"sublime_text", "subl"}},
        {"atom", {"atom"}},
        {"vim", {"vim", "nvim", "gvim"}},
        {"nano", {"nano"}},
        {"notepad", {"notepad", "notepad.exe"}},
        
        // Browsers
        {"chrome", {"chrome", "google-chrome", "chrome.exe"}},
        {"firefox", {"firefox", "firefox.exe"}},
        {"edge", {"msedge", "microsoftedge", "edge"}},
        {"brave", {"brave", "brave-browser"}},
        
        // Development Tools
        {"git", {"git"}},
        {"node", {"node", "nodejs"}},
        {"python", {"python", "python3", "py"}},
        {"java", {"java"}},
        {"javac", {"javac"}},
        {"gcc", {"gcc", "g++"}},
        {"make", {"make", "mingw32-make"}},
        {"cmake", {"cmake"}},
        
        // System Tools
        {"explorer", {"explorer", "explorer.exe"}},
        {"cmd", {"cmd", "cmd.exe"}},
        {"powershell", {"powershell", "pwsh"}},
        {"pwsh", {"pwsh"}},
        {"regedit", {"regedit", "regedit.exe"}},
        {"taskmgr", {"taskmgr", "taskmgr.exe"}},
        {"calc", {"calc", "calc.exe"}},
        {"mspaint", {"mspaint", "mspaint.exe"}},
        
        // Media
        {"vlc", {"vlc"}},
        {"spotify", {"spotify"}},
        {"discord", {"discord"}},
        
        // Office
        {"word", {"winword", "word"}},
        {"excel", {"excel"}},
        {"powerpoint", {"powerpnt"}},
        
        // IDEs
        {"visual-studio", {"devenv"}},
        {"intellij", {"idea", "idea64"}},
        {"eclipse", {"eclipse"}},
        {"android-studio", {"studio", "studio64"}},
    };
    
    std::string app_name;
    std::string path = ".";
    
    if (args.size() == 1) {
        // No arguments - try to open current directory in default editor
        app_name = "vscode";  // Default to VS Code
    } else if (args.size() == 2) {
        // One argument - could be app name or path
        std::string arg = args[1];
        if (app_shortcuts.contains(arg)) {
            app_name = arg;
        } else {
            // Assume it's a path, use default editor
            app_name = "vscode";
            path = expand_path(arg);
        }
    } else {
        // Two or more arguments - first is app, rest is path/args
        app_name = args[1];
        if (args.size() > 2) {
            path = expand_path(args[2]);
        }
    }
    
    // Special case: if path is "." and app is a system tool, don't pass path
    bool pass_path = true;
    std::vector<std::string> no_path_apps = {"taskmgr", "calc", "regedit", "mspaint", "cmd", "powershell", "pwsh"};
    if (path == "." && std::find(no_path_apps.begin(), no_path_apps.end(), app_name) != no_path_apps.end()) {
        pass_path = false;
    }
    
    // Determine if app should be detached or visible
    std::vector<std::string> interactive_apps = {"cmd", "powershell", "pwsh", "python", "node", "java"};
    bool is_interactive = std::find(interactive_apps.begin(), interactive_apps.end(), app_name) != interactive_apps.end();
    
    // Try to find and launch the application
    if (app_shortcuts.contains(app_name)) {
        for (const auto& executable_name : app_shortcuts[app_name]) {
            std::string executable = find_executable(executable_name);
            if (!executable.empty()) {
                std::string command;
                if (pass_path && path != ".") {
                    command = std::format("\"{}\" \"{}\"", executable, path);
                } else if (pass_path) {
                    command = std::format("\"{}\" .", executable);
                } else {
                    command = std::format("\"{}\"", executable);
                }
                
                STARTUPINFOA si = { sizeof(si) };
                PROCESS_INFORMATION pi = { 0 };
                
                DWORD creation_flags = is_interactive ? CREATE_NEW_CONSOLE : DETACHED_PROCESS;
                
                if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 
                                 creation_flags, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    
                    ColorGuard guard(theme.success_color);
                    if (pass_path) {
                        std::cout << std::format("Opened {} in {}\n", path, executable_name);
                    } else {
                        std::cout << std::format("Launched {}\n", executable_name);
                    }
                    return 0;
                }
            }
        }
        
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: {} not found. Make sure it's installed and in PATH.\n", app_name);
        return 1;
    } else {
        // Try to launch as direct executable name
        std::string executable = find_executable(app_name);
        if (!executable.empty()) {
            std::string command;
            if (args.size() > 2) {
                // Pass all remaining arguments
                command = std::format("\"{}\"", executable);
                for (size_t i = 2; i < args.size(); ++i) {
                    command += std::format(" \"{}\"", args[i]);
                }
            } else {
                command = std::format("\"{}\"", executable);
            }
            
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            
            DWORD creation_flags = is_interactive ? CREATE_NEW_CONSOLE : DETACHED_PROCESS;
            
            if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 
                             creation_flags, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                
                ColorGuard guard(theme.success_color);
                std::cout << std::format("Launched {}\n", app_name);
                return 0;
            }
        }
        
        ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: '{}' not found.\n", app_name);
        std::cout << "\nSupported shortcuts:\n";
        std::cout << "Editors: vscode, notepad++, sublime, atom, vim, notepad\n";
        std::cout << "Browsers: chrome, firefox, edge, brave\n";
        std::cout << "Tools: explorer, cmd, powershell, taskmgr, calc, regedit\n";
        std::cout << "Dev: git, node, python, java, gcc, make, cmake\n";
        std::cout << "Or use any executable name directly.\n";
        return 1;
    }
}

int edit(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: edit <filename>\n";
        return 1;
    }
    
    std::string filename = expand_path(args[1]);
    const Theme theme;
    
    // Try external editors first (much more reliable)
    std::vector<std::string> editors = {
        "notepad",      // Always available on Windows
        "notepad++",    // Popular editor
        "code",         // VS Code
        "vim",          // If available
        "nano"          // If available
    };
    
    for (const auto& editor : editors) {
        std::string executable = find_executable(editor);
        if (!executable.empty()) {
            std::string command = std::format("\"{}\" \"{}\"", executable, filename);
            
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            
            // Create file if it doesn't exist to avoid permission issues
            if (!fs::exists(filename)) {
                std::ofstream create_file(filename);
                if (!create_file) {
                    ColorGuard error_guard(theme.error_color);
                    std::cerr << std::format("Cannot create file: {}\n", filename);
                    continue;
                }
            }
            
            // Launch and wait for editor to close
            if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 
                             0, nullptr, nullptr, &si, &pi)) {
                
                ColorGuard guard(theme.success_color);
                std::cout << std::format("Opening {} in {}...\n", filename, editor);
                
                // Wait for the editor to close
                WaitForSingleObject(pi.hProcess, INFINITE);
                
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                
                std::cout << "Editor closed.\n";
                return 0;
            }
        }
    }
    
    // Fallback: Very simple built-in editor using standard input
    ColorGuard guard(theme.warning_color);
    std::cout << "No external editor found. Using simple built-in editor.\n";
    std::cout << std::format("Editing: {}\n", filename);
    
    std::vector<std::string> lines;
    bool file_exists = fs::exists(filename);
    
    // Load existing file
    if (file_exists) {
        std::ifstream file(filename);
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        std::cout << std::format("Loaded {} lines\n", lines.size());
    } else {
        std::cout << "Creating new file\n";
    }
    
    std::cout << "\nSimple Editor - Commands:\n";
    std::cout << "  SAVE  - Save file and exit\n";
    std::cout << "  QUIT  - Exit without saving\n";
    std::cout << "  LIST  - Show all lines\n";
    std::cout << "  HELP  - Show this help\n";
    std::cout << "\nEnter lines of text:\n\n";
    
    bool modified = false;
    
    while (true) {
        std::cout << std::format("Line {}: ", lines.size() + 1);
        
        // Use simple standard input (no fancy line editing)
        std::string input;
        if (!std::getline(std::cin, input)) {
            break; // EOF
        }
        
        if (input == "SAVE") {
            std::ofstream file(filename);
            if (file) {
                for (const auto& line : lines) {
                    file << line << '\n';
                }
                ColorGuard save_guard(theme.success_color);
                std::cout << std::format("Saved {} ({} lines)\n", filename, lines.size());
            } else {
                ColorGuard error_guard(theme.error_color);
                std::cerr << std::format("Error: Cannot write to {}\n", filename);
            }
            break;
        } else if (input == "QUIT") {
            if (modified) {
                std::cout << "File has unsaved changes. Type 'SAVE' to save first.\n";
                continue;
            }
            std::cout << "Exiting without saving.\n";
            break;
        } else if (input == "LIST") {
            std::cout << "\nFile contents:\n";
            for (size_t i = 0; i < lines.size(); ++i) {
                std::cout << std::format("{:3}: {}\n", i + 1, lines[i]);
            }
            std::cout << '\n';
        } else if (input == "HELP") {
            std::cout << "\nCommands:\n";
            std::cout << "  SAVE  - Save file and exit\n";
            std::cout << "  QUIT  - Exit without saving\n";
            std::cout << "  LIST  - Show all lines\n";
            std::cout << "  HELP  - Show this help\n\n";
        } else {
            // Add line to file
            lines.push_back(input);
            modified = true;
        }
    }
    
    return 0;
}

int vi(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        const Theme theme;
        ColorGuard guard(theme.error_color);
        std::cerr << "jshell: Usage: vi <filename>\n";
        return 1;
    }
    
    std::string filename = expand_path(args[1]);
    
    // Add .txt extension if no extension provided
    if (filename.find('.') == std::string::npos) {
        filename += ".txt";
    }
    
    const Theme theme;
    
    std::vector<std::string> lines;
    bool file_exists = fs::exists(filename);
    
    // Load existing file
    if (file_exists) {
        std::ifstream file(filename);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                lines.push_back(line);
            }
            file.close();
        }
    }
    
    if (lines.empty()) {
        lines.push_back(""); // At least one empty line
    }
    
    // Don't clear screen - just show editor inline
    std::cout << "\n";
    ColorGuard header_guard(theme.prompt_color);
    std::cout << R"(
    ========================================
    ||            VI EDITOR               ||
    ========================================)" << '\n';
    
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
    std::cout << std::format("    File: {} ({} lines)\n", filename, lines.size());
    
    // Show file content with line numbers
    for (size_t i = 0; i < lines.size(); ++i) {
        std::cout << std::format("{:3}: {}\n", i + 1, lines[i]);
    }
    
    std::cout << std::string(40, '-') << '\n';
    std::cout << "Commands: (i)nsert, (e)dit line, (d)elete, (s)ave, (q)uit, (l)ist, (h)elp\n";
    
    bool modified = false;
    size_t current_line = 0;
    
    while (true) {
        std::cout << std::format("vi:{} ", current_line + 1);
        std::string input;
        if (!std::getline(std::cin, input) || input.empty()) {
            continue;
        }
        
        char command = std::tolower(input[0]);
        
        switch (command) {
            case 'i': { // Insert mode at current position
                std::cout << std::format("Insert at line {} (empty line to exit):\n", current_line + 1);
                std::vector<std::string> new_lines;
                while (true) {
                    std::cout << "+ ";
                    std::string line;
                    if (!std::getline(std::cin, line)) break;
                    if (line.empty()) break;
                    new_lines.push_back(line);
                }
                
                // Insert new lines at current position
                lines.insert(lines.begin() + current_line, new_lines.begin(), new_lines.end());
                current_line += new_lines.size();
                modified = true;
                
                std::cout << std::format("Inserted {} lines.\n", new_lines.size());
                break;
            }
            case 'e': { // Edit specific line
                if (input.length() > 2) {
                    // Parse line number from command like "e5"
                    try {
                        size_t line_num = std::stoul(input.substr(1)) - 1;
                        if (line_num < lines.size()) {
                            current_line = line_num;
                            std::cout << std::format("Current: {}: {}\n", current_line + 1, lines[current_line]);
                            std::cout << "New text: ";
                            std::string new_text;
                            if (std::getline(std::cin, new_text)) {
                                lines[current_line] = new_text;
                                modified = true;
                                std::cout << "Line updated.\n";
                            }
                        } else {
                            std::cout << "Invalid line number.\n";
                        }
                    } catch (...) {
                        std::cout << "Usage: e<line_number> (e.g., e5)\n";
                    }
                } else {
                    std::cout << std::format("Current: {}: {}\n", current_line + 1, lines[current_line]);
                    std::cout << "New text: ";
                    std::string new_text;
                    if (std::getline(std::cin, new_text)) {
                        lines[current_line] = new_text;
                        modified = true;
                        std::cout << "Line updated.\n";
                    }
                }
                break;
            }
            case 'd': { // Delete line
                if (input.length() > 1) {
                    try {
                        size_t line_num = std::stoul(input.substr(1)) - 1;
                        if (line_num < lines.size()) {
                            std::cout << std::format("Deleting: {}: {}\n", line_num + 1, lines[line_num]);
                            lines.erase(lines.begin() + line_num);
                            modified = true;
                            if (current_line >= lines.size() && current_line > 0) {
                                current_line = lines.size() - 1;
                            }
                        } else {
                            std::cout << "Invalid line number.\n";
                        }
                    } catch (...) {
                        std::cout << "Usage: d<line_number> (e.g., d5)\n";
                    }
                } else {
                    if (current_line < lines.size()) {
                        std::cout << std::format("Deleting: {}: {}\n", current_line + 1, lines[current_line]);
                        lines.erase(lines.begin() + current_line);
                        modified = true;
                        if (current_line >= lines.size() && current_line > 0) {
                            current_line = lines.size() - 1;
                        }
                    }
                }
                break;
            }
            case 'j': { // Move down
                if (current_line < lines.size() - 1) {
                    current_line++;
                    std::cout << std::format("{}: {}\n", current_line + 1, lines[current_line]);
                }
                break;
            }
            case 'k': { // Move up
                if (current_line > 0) {
                    current_line--;
                    std::cout << std::format("{}: {}\n", current_line + 1, lines[current_line]);
                }
                break;
            }
            case 'g': { // Go to line
                if (input.length() > 1) {
                    try {
                        size_t line_num = std::stoul(input.substr(1)) - 1;
                        if (line_num < lines.size()) {
                            current_line = line_num;
                            std::cout << std::format("{}: {}\n", current_line + 1, lines[current_line]);
                        } else {
                            std::cout << "Invalid line number.\n";
                        }
                    } catch (...) {
                        std::cout << "Usage: g<line_number> (e.g., g5)\n";
                    }
                } else {
                    current_line = 0; // Go to first line
                    std::cout << std::format("{}: {}\n", current_line + 1, lines[current_line]);
                }
                break;
            }
            case 'l': { // List all lines
                std::cout << "\n File contents:\n";
                std::cout << std::string(50, '-') << '\n';
                for (size_t i = 0; i < lines.size(); ++i) {
                    char marker = (i == current_line) ? '>' : ' ';
                    ColorGuard line_guard(i == current_line ? theme.success_color : theme.default_color);
                    std::cout << std::format("{}{:3}: {}\n", marker, i + 1, lines[i]);
                }
                std::cout << std::string(50, '-') << '\n';
                std::cout << std::format("Current line: {} of {}\n\n", current_line + 1, lines.size());
                break;
            }
            case 's': { // Save
                std::ofstream file(filename);
                if (file) {
                    for (const auto& line : lines) {
                        file << line << '\n';
                    }
                    ColorGuard save_guard(theme.success_color);
                    std::cout << std::format("Saved {} ({} lines)\n", filename, lines.size());
                    modified = false;
                } else {
                    ColorGuard error_guard(theme.error_color);
                    std::cerr << std::format("Error: Cannot write to {}\n", filename);
                }
                break;
            }
            case 'q': { // Quit
                if (modified) {
                    std::cout << "File has unsaved changes. Save first? (y/n): ";
                    std::string confirm;
                    if (std::getline(std::cin, confirm) && std::tolower(confirm[0]) == 'y') {
                        std::ofstream file(filename);
                        if (file) {
                            for (const auto& line : lines) {
                                file << line << '\n';
                            }
                            std::cout << "Saved and exiting.\n";
                        }
                    }
                }
                std::cout << "=== Vi Editor Closed ===\n\n";
                return 0;
            }
            case 'h': { // Help
                std::cout << "\nVi Editor Commands:\n";
                std::cout << "  i       - Insert mode at current line\n";
                std::cout << "  e[N]    - Edit line N (or current line)\n";
                std::cout << "  d[N]    - Delete line N (or current line)\n";
                std::cout << "  j       - Move down one line\n";
                std::cout << "  k       - Move up one line\n";
                std::cout << "  g[N]    - Go to line N (or first line)\n";
                std::cout << "  l       - List all lines with current position\n";
                std::cout << "  s       - Save file\n";
                std::cout << "  q       - Quit (prompts to save if modified)\n";
                std::cout << "  h       - Show this help\n\n";
                break;
            }
            default:
                std::cout << "Unknown command. Type 'h' for help.\n";
                break;
        }
    }
    
    return 0;
}

int version(ShellState&, std::span<const char*>) {
    const Theme theme;
    ColorGuard header_guard(theme.prompt_color);
    std::cout << "jshell v0.0 - Enhanced C++ Shell for Windows\n";
    std::cout << "Built with caffeine & C++ by Camresh - CNJMTechnologies INC\n";
    
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
    std::cout << "Built with: g++ (MinGW) C++20\n";
    std::cout << "Copyright (c) future\n";
    std::cout << "Built with caffeine & C++ by Camresh - CNJMTechnologies INC\n";
    
    return 0;
}

// --- Main Execution Logic ---
int execute(ShellState& state, std::vector<Command>& commands) {
    if (commands.empty() || commands[0].args.empty()) {
        return 0;
    }

    std::string cmd_name = commands[0].args[0];
    if (state.aliases.contains(cmd_name)) {
        std::string alias_cmd = state.aliases[cmd_name];
        std::vector<std::string> original_args(commands[0].args.begin() + 1, commands[0].args.end());
        
        auto alias_tokens = tokenize(alias_cmd);
        commands[0].args = alias_tokens;
        commands[0].args.insert(commands[0].args.end(), original_args.begin(), original_args.end());
    }

    if (commands.size() == 1) {
        for (const auto& builtin : builtins) {
            if (commands[0].args[0] == builtin.name) {
                std::vector<const char*> c_args;
                c_args.reserve(commands[0].args.size());
                for (const auto& s : commands[0].args) c_args.push_back(s.c_str());
                int result = builtin.func(state, c_args);
                state.last_exit_code = result;
                return result;
            }
        }
        
        int result = launch_process(commands[0], INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, &state);
        state.last_exit_code = result;
        return result;
    }
    
    int num_pipes = static_cast<int>(commands.size()) - 1;
    std::vector<ScopedHandle> pipe_read(num_pipes);
    std::vector<ScopedHandle> pipe_write(num_pipes);
    
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

    for (int i = 0; i < num_pipes; ++i) {
        HANDLE read_handle, write_handle;
        if (!CreatePipe(&read_handle, &write_handle, &sa, MAX_PIPE_BUFFER)) {
            const Theme theme;
            ColorGuard guard(theme.error_color);
            std::cerr << "jshell: CreatePipe failed\n";
            return 1;
        }
        pipe_read[i].reset(read_handle);
        pipe_write[i].reset(write_handle);
    }

    std::vector<std::thread> threads;
    std::vector<int> exit_codes(commands.size());
    std::mutex mtx;

    for (size_t i = 0; i < commands.size(); ++i) {
        threads.emplace_back([&, i]() {
            HANDLE hInput = (i == 0) ? INVALID_HANDLE_VALUE : pipe_read[i - 1].get();
            HANDLE hOutput = (i == commands.size() - 1) ? INVALID_HANDLE_VALUE : pipe_write[i].get();
            
            bool is_builtin = false;
            for (const auto& builtin : builtins) {
                if (commands[i].args[0] == builtin.name) {
                    is_builtin = true;
                    // Note: Builtin redirection in pipelines is complex and not fully supported here.
                    std::vector<const char*> c_args;
                    for (const auto& s : commands[i].args) c_args.push_back(s.c_str());
                    
                    std::lock_guard<std::mutex> lock(mtx);
                    exit_codes[i] = builtin.func(state, c_args);
                    break;
                }
            }

            if (!is_builtin) {
                exit_codes[i] = launch_process(commands[i], hInput, hOutput, INVALID_HANDLE_VALUE, nullptr);
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    state.last_exit_code = exit_codes.back();
    return state.last_exit_code;
}

void load_config(ShellState& state) {
    fs::path config_path = state.shell_directory / "config.ini";
    
    if (!fs::exists(config_path)) return;
    
    std::ifstream file(config_path);
    std::string line;
    
    while (std::getline(file, line)) {
        if (line.empty() || line.starts_with('#')) continue;
        
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);
        
        if (key == "prompt_format") state.config.prompt_format = value;
        else if (key == "enable_colors") state.config.enable_colors = (value == "true" || value == "1");
        else if (key == "auto_complete") state.config.auto_complete = (value == "true" || value == "1");
        else if (key == "save_history") state.config.save_history = (value == "true" || value == "1");
        else if (key == "max_history") { try { state.config.max_history = std::stoul(value); } catch (...) {} }
    }
}

void initialize_shell(ShellState& state) {
    load_config(state);
    load_history(state);
    
    fs::path rc_file = state.shell_directory / ".jshellrc";
    if (fs::exists(rc_file)) {
        std::vector<const char*> source_args = {"source", rc_file.string().c_str()};
        source(state, source_args);
    }
    
    std::string home = get_home_directory();
    if (!home.empty()) {
        fs::path home_rc = fs::path(home) / ".jshellrc";
        if (fs::exists(home_rc) && home_rc != rc_file) {
            std::vector<const char*> source_args = {"source", home_rc.string().c_str()};
            source(state, source_args);
        }
    }
}

void shell_loop() {
    ShellState state;
    const Theme theme;

    initialize_shell(state);
    
    // Cool ASCII Art Banner
    if (state.config.enable_colors) {
        ColorGuard guard(theme.prompt_color);
        std::cout << R"(
   __        _            _  _ 
   \ \  ___ | |__    ___ | || |
    \ \/ __|| '_ \  / _ \| || |
 /\_/ /\__ \| | | ||  __/| || |
 \___/ |___/|_| |_| \___||_||_|
                               
)" << '\n';
        
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), theme.default_color);
        std::cout << "        Enhanced C++ Shell for Windows v2.0\n";
        std::cout << "    Built with caffeine & C++ by Camresh - CNJMTechnologies INC\n";
        
        ColorGuard feature_guard(theme.help_command_color);
        std::cout << "\n  <Features: Job Control | Pipes | Redirection | Vi Editor >\n";
    } else {
        std::cout << R"(
   __        _            _  _ 
   \ \  ___ | |__    ___ | || |
    \ \/ __|| '_ \  / _ \| || |
 /\_/ /\__ \| | | ||  __/| || |
 \___/ |___/|_| |_| \___||_||_|
                               
)" << '\n';
        std::cout << "        Enhanced C++ Shell for Windows v2.0\n";
        std::cout << "    Built with caffeine & C++ by Camresh - CNJMTechnologies INC\n";
        std::cout << "\n    Features: Job Control | Pipes | Redirection | Vi Editor\n";
    }
    std::cout << "Type 'help' for available commands.\n\n";

    while (state.running) {
        try {
            std::string line = read_line(state);
            if (line.empty()) continue;
            
            auto commands = parse_pipeline(line, state);
            if (!commands.empty()) {
                execute(state, commands);
            }
        } catch (const std::exception& e) {
            if (state.config.enable_colors) {
                ColorGuard guard(theme.error_color);
                std::cerr << std::format("jshell: Error: {}\n", e.what());
            } else {
                std::cerr << std::format("jshell: Error: {}\n", e.what());
            }
        }
    }
    
    save_history(state);
}

} // namespace jshell

void generate_nsis_script() {
    std::ofstream nsis("jshell_installer.nsi");
    nsis << R"(!define APPNAME "jshell"
!define COMPANYNAME "CNJMTechnologies INC [https://cnjm-technologies-inc.vercel.app]"
!define DESCRIPTION "Enhanced C++ JShell for Windows"
!define VERSIONMAJOR 0
!define VERSIONMINOR 0
!define VERSIONBUILD 0

RequestExecutionLevel admin
InstallDir "C:\${APPNAME}"  ; Install to root directory
Name "${APPNAME}"
OutFile "${APPNAME}-installer.exe"
Icon "jshell-icon.ico"           ; Installer icon
UninstallIcon "jshell-icon.ico"  ; Uninstaller icon
BrandingText "By Camresh - CNJMTechnologies INC"
!include LogicLib.nsh
!include WinMessages.nsh

Page license
Page directory
Page instfiles

LicenseData "license.txt"



Section "install"
    ; Check if already installed
    ReadRegStr $R0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "UninstallString"
    ${If} $R0 != ""
        MessageBox MB_YESNO "${APPNAME} is already installed. Uninstall first?" IDYES uninstall_first
        Abort
        uninstall_first:
            ExecWait '$R0'
    ${EndIf}
    
    SetOutPath $INSTDIR
    File "jshell.exe"
    File "jshell-icon.ico"       ; Copy icon to installation directory
    File "license.txt"           ; Copy license to installation directory
    File "INSTALLATION_NOTES.txt" ; Copy installation instructions
    WriteUninstaller "$INSTDIR\uninstall.exe"
    
    ; Create Start Menu folder and shortcuts with icon
    CreateDirectory "$SMPROGRAMS\${APPNAME}"
    CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\jshell.exe" "" "$INSTDIR\jshell-icon.ico"
    CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\jshell.exe" "" "$INSTDIR\jshell-icon.ico"
    
    ; NOTE: PATH is not automatically modified by this installer
    ; Users can manually add C:\jshell to their PATH if desired
    
    ; Registry entries for Add/Remove Programs
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayName" "${APPNAME} - ${DESCRIPTION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayIcon" "$INSTDIR\jshell-icon.ico"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "Publisher" "${COMPANYNAME}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayVersion" "${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoRepair" 1
SectionEnd

Section "uninstall"
    ; Remove shortcuts
    Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
    RMDir "$SMPROGRAMS\${APPNAME}"
    Delete "$DESKTOP\${APPNAME}.lnk"
    
    ; NOTE: PATH is not modified by this installer
    ; Users must manually remove C:\jshell from PATH if they added it
    
    ; Remove files
    Delete "$INSTDIR\jshell.exe"
    Delete "$INSTDIR\jshell-icon.ico"
    Delete "$INSTDIR\license.txt"
    Delete "$INSTDIR\INSTALLATION_NOTES.txt"
    Delete "$INSTDIR\uninstall.exe"
    RMDir $INSTDIR
    
    ; Remove registry entries
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
SectionEnd)";
    std::cout << "Generated NSIS installer script: jshell_installer.nsi\n";
}

// --- Main Function ---
int main(int argc, char** argv) {
   if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--generate-nsis") {
            generate_nsis_script();
            return 0; 
        }
        if (arg1 == "--version") {
            jshell::ShellState dummy_state;
            jshell::version(dummy_state, {});
            return 0; 
        }
    }

   try {
        if (argc > 1) {
           jshell::ShellState state;
            jshell::initialize_shell(state);
            std::vector<const char*> script_args = {"source", argv[1]};
            return jshell::source(state, script_args);
        } else {
            jshell::shell_loop();
        }
    } catch (const std::exception& e) {
        const jshell::Theme theme;
        jshell::ColorGuard guard(theme.error_color);
        std::cerr << std::format("jshell: Fatal error: {}\n", e.what());
        return 1;
    }
    
    return 0;
}