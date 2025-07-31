#include <iostream>      // For std::cout, std::cerr, std::getline
#include <string>        // For std::string, std::string_view
#include <vector>        // For std::vector
#include <span>          // For std::span (C++20)
#include <format>        // For std::format (C++20)
#include <memory>        // For std::unique_ptr
#include <windows.h>     // For Windows API
#include <filesystem>    // For modern file system operations (C++17)
#include <stdexcept>     // For std::runtime_error
#include <fstream>       // For file redirection and scripting
#include <conio.h>       // For _getch
#include <algorithm>     // For std::find_if, std::min, etc.
#include <map>           // For aliases
#include <sstream>       // For parsing
#include <chrono>        // For file modification times
#include <numeric>       // For std::accumulate

namespace fs = std::filesystem;

namespace jshell {

// --- 1. Core Structs & Type Definitions ---

constexpr size_t JSHELL_HISTORY_SIZE = 100;

struct Theme {
    WORD default_color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    WORD prompt_color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    WORD error_color = FOREGROUND_RED | FOREGROUND_INTENSITY;
    WORD dir_color = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    WORD help_command_color = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
};

struct ShellState {
    std::vector<std::string> history;
    size_t history_index = 0;
    std::map<std::string, std::string> aliases;
    bool running = true;
};

struct Command {
    std::vector<std::string> args;
    std::string input_file;
    std::string output_file;
};

struct Builtin {
    const char* name;
    int (*func)(ShellState&, std::span<const char*>);
    const char* description;
};

// --- 2. Forward Declarations for All Built-in Functions ---

int cd(ShellState&, std::span<const char*>);
int help(ShellState&, std::span<const char*>);
int exit_shell(ShellState&, std::span<const char*>);
int pwd(ShellState&, std::span<const char*>);
int env(ShellState&, std::span<const char*>);
int set_env(ShellState&, std::span<const char*>);
int unset_env(ShellState&, std::span<const char*>);
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

// --- 3. The Global Built-ins Table ---

const std::vector<Builtin> builtins = {
    {"cd",      cd,      "Change directory. `cd` or `cd ~` goes home."},
    {"help",    help,    "Display this help message."},
    {"exit",    exit_shell,"Exit the shell."},
    {"pwd",     pwd,     "Print the current working directory."},
    {"env",     env,     "List all environment variables."},
    {"set",     set_env, "Set an env variable. Usage: set <NAME> <VALUE>"},
    {"unset",   unset_env,"Unset an env variable. Usage: unset <NAME>"},
    {"history", history, "Show command history."},
    {"source",  source,  "Execute a script file. Usage: source <script.jsh>"},
    {"ls",      ls,      "List directory contents. Flags: -l (long), -a (all)."},
    {"dir",     ls,      "Alias for ls."},
    {"cat",     cat,     "Display file contents. Usage: cat <file>"},
    {"echo",    echo,    "Display a line of text."},
    {"mkdir",   mkdir,   "Create a directory. Usage: mkdir <directory>"},
    {"rm",      rm,      "Remove a file or directory. Usage: rm <path>"},
    {"del",     rm,      "Alias for rm."},
    {"cls",     cls,     "Clear the console screen."},
    {"clear",   cls,     "Alias for cls."},
    {"alias",   alias,   "Create command alias. Usage: alias name='command'"},
    {"unalias", unalias, "Remove a command alias. Usage: unalias <name>"},
    {"touch",   touch,   "Create an empty file. Usage: touch <file>"},
};

// --- 4. Function Definitions ---

void set_console_color(WORD attributes) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), attributes);
}

std::string get_user_profile_directory() {
    char* path = nullptr;
    size_t len;
    if (_dupenv_s(&path, &len, "USERPROFILE") == 0 && path != nullptr) {
        std::string user_profile = path;
        free(path);
        return user_profile;
    }
    return "";
}

std::string get_current_directory_prompt() {
    try {
        std::string home = get_user_profile_directory();
        std::string current = fs::current_path().string();
        if (!home.empty() && current.starts_with(home)) {
            return "~" + current.substr(home.length());
        }
        return current;
    } catch (const fs::filesystem_error&) {
        return "unknown";
    }
}

std::string find_longest_common_prefix(const std::vector<std::string>& strs) {
    if (strs.empty()) return "";
    return std::accumulate(strs.begin() + 1, strs.end(), strs[0],
        [](std::string const& prefix, std::string const& s) {
            auto mismatch_pair = std::mismatch(prefix.begin(), prefix.end(), s.begin(), s.end());
            return std::string(prefix.begin(), mismatch_pair.first);
        });
}

std::vector<std::string> get_completions(const std::string& prefix) {
    std::vector<std::string> completions;
    fs::path current_path = ".";
    std::string search_prefix = prefix;

    if (auto pos = prefix.find_last_of("/\\"); pos != std::string::npos) {
        current_path = prefix.substr(0, pos + 1);
        search_prefix = prefix.substr(pos + 1);
    }
    
    try {
        if(fs::exists(current_path) && fs::is_directory(current_path)) {
            for (const auto& entry : fs::directory_iterator(current_path)) {
                std::string filename = entry.path().filename().string();
                if (filename.starts_with(search_prefix)) {
                    std::string completion = (current_path.string() == "." ? "" : current_path.string()) + filename;
                    completions.push_back(completion);
                }
            }
        }
    } catch(...) { /* Ignore errors */ }

    for (const auto& builtin : builtins) {
        if (std::string(builtin.name).starts_with(prefix)) {
            completions.push_back(builtin.name);
        }
    }
    std::sort(completions.begin(), completions.end());
    return completions;
}

void redraw_line(const std::string& prompt, const std::string& line) {
    std::cout << "\r" << std::string(prompt.length() + line.length() + 20, ' ') << "\r"; // Clear generously
    const Theme theme;
    set_console_color(theme.prompt_color);
    std::cout << prompt;
    set_console_color(theme.default_color);
    std::cout << line;
    std::cout.flush();
}

std::string read_line(ShellState& state) {
    std::string prompt = std::format("[{}] > ", get_current_directory_prompt());
    redraw_line(prompt, "");

    std::string line;
    int last_char = 0;

    while (true) {
        int ch = _getch();
        if (ch == 13) { // Enter
            std::cout << "\n";
            break;
        } else if (ch == 224) { // Arrow keys
             ch = _getch();
             if (ch == 72 && !state.history.empty()) { // Up
                if (state.history_index > 0) {
                    state.history_index--;
                    line = state.history[state.history_index];
                    redraw_line(prompt, line);
                }
             } else if (ch == 80) { // Down
                if (state.history_index < state.history.size()) {
                    state.history_index++;
                    line = (state.history_index < state.history.size()) ? state.history[state.history_index] : "";
                    redraw_line(prompt, line);
                }
             }
        } else if (ch == 9) { // Tab
            auto completions = get_completions(line);
            if (completions.empty()) continue;

            if (completions.size() == 1) {
                line = completions[0];
            } else {
                std::string lcp = find_longest_common_prefix(completions);
                if (!lcp.empty() && lcp.length() > line.length()) {
                    line = lcp;
                } else if (last_char == 9) { // Second tab press lists options
                    std::cout << "\n";
                    for(const auto& c : completions) std::cout << c << "\t";
                    std::cout << "\n";
                    redraw_line(prompt, line);
                }
            }
            redraw_line(prompt, line);
        } else if (ch == 8 && !line.empty()) { // Backspace
            line.pop_back();
            std::cout << "\b \b";
        } else if (isprint(ch)) { // Printable characters
            line += static_cast<char>(ch);
            std::cout << static_cast<char>(ch);
        }
        last_char = ch;
        std::cout.flush();
    }

    if (!line.empty() && (state.history.empty() || state.history.back() != line)) {
        if (state.history.size() >= JSHELL_HISTORY_SIZE) {
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
    for (char c : str) {
        if (c == '"') {
            in_quotes = !in_quotes;
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

Command parse_single_command(std::string_view command_str) {
    Command cmd;
    std::string temp_str(command_str);
    size_t pos_out = temp_str.find('>');
    size_t pos_in = temp_str.find('<');

    if (pos_out != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_out + 1));
        if(!tokens.empty()) cmd.output_file = tokens[0];
        temp_str = temp_str.substr(0, pos_out);
    }
    if (pos_in != std::string::npos) {
        auto tokens = tokenize(temp_str.substr(pos_in + 1));
        if(!tokens.empty()) cmd.input_file = tokens[0];
        temp_str = temp_str.substr(0, pos_in);
    }
    
    cmd.args = tokenize(temp_str);
    return cmd;
}

std::vector<Command> split_line(const std::string& line) {
    if (line.empty()) return {};
    std::vector<Command> commands;
    std::string current_segment;
    std::stringstream ss(line);
    while (std::getline(ss, current_segment, '|')) {
        commands.push_back(parse_single_command(current_segment));
    }
    return commands;
}

struct ParsedArgs {
    std::map<char, bool> flags;
    std::vector<std::string> non_flag_args;
};

ParsedArgs parse_args(std::span<const char*> args) {
    ParsedArgs result;
    for (size_t i = 1; i < args.size(); ++i) { // Skip command name
        std::string arg = args[i];
        if (arg.starts_with('-') && arg.length() > 1) {
            for (size_t j = 1; j < arg.length(); ++j) {
                result.flags[arg[j]] = true;
            }
        } else {
            result.non_flag_args.push_back(arg);
        }
    }
    return result;
}

int launch(Command& cmd, HANDLE hInput, HANDLE hOutput) {
    if (cmd.args.empty()) return 1;

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    HANDLE hInputFile = INVALID_HANDLE_VALUE;
    HANDLE hOutputFile = INVALID_HANDLE_VALUE;
    const Theme theme;

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = (hInput != INVALID_HANDLE_VALUE) ? hInput : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = (hOutput != INVALID_HANDLE_VALUE) ? hOutput : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (!cmd.input_file.empty()) {
        hInputFile = CreateFileA(cmd.input_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hInputFile == INVALID_HANDLE_VALUE) {
            set_console_color(theme.error_color);
            std::cerr << std::format("jshell: Failed to open input file '{}': {}\n", cmd.input_file, std::system_category().message(GetLastError()));
            set_console_color(theme.default_color);
            return 1;
        }
        si.hStdInput = hInputFile;
    }
    if (!cmd.output_file.empty()) {
        hOutputFile = CreateFileA(cmd.output_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOutputFile == INVALID_HANDLE_VALUE) {
            set_console_color(theme.error_color);
            std::cerr << std::format("jshell: Failed to open output file '{}': {}\n", cmd.output_file, std::system_category().message(GetLastError()));
            set_console_color(theme.default_color);
            if (hInputFile != INVALID_HANDLE_VALUE) CloseHandle(hInputFile);
            return 1;
        }
        si.hStdOutput = hOutputFile;
    }

    std::string cmd_line;
    for (const auto& arg : cmd.args) {
        cmd_line += (arg.find(' ') != std::string::npos) ? ("\"" + arg + "\" ") : (arg + " ");
    }

    if (!CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        set_console_color(theme.error_color);
        std::cerr << std::format("jshell: Command not found or failed to execute: '{}'\n", cmd.args[0]);
        set_console_color(theme.default_color);
        if (hInputFile != INVALID_HANDLE_VALUE) CloseHandle(hInputFile);
        if (hOutputFile != INVALID_HANDLE_VALUE) CloseHandle(hOutputFile);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hInputFile != INVALID_HANDLE_VALUE) CloseHandle(hInputFile);
    if (hOutputFile != INVALID_HANDLE_VALUE) CloseHandle(hOutputFile);
    return 1;
}

int execute(ShellState& state, std::vector<Command>& commands); // Forward declare for source command

// --- Built-in Command Implementations ---

int cd(ShellState&, std::span<const char*> args) {
    if (args.size() < 2 || (args.size() > 1 && std::string(args[1]) == "~")) {
        std::string home = get_user_profile_directory();
        if (!home.empty()) {
            try { fs::current_path(home); } catch (...) {}
        } else {
            std::cerr << "jshell: HOME directory not found\n";
        }
    } else {
        try {
            fs::current_path(args[1]);
        } catch (const fs::filesystem_error& e) {
            const Theme theme;
            set_console_color(theme.error_color);
            std::cerr << std::format("jshell: cd: {}\n", e.what());
            set_console_color(theme.default_color);
        }
    }
    return 1;
}

// ========================================================================
// === THIS IS THE FIXED FUNCTION ===
// ========================================================================
int help(ShellState&, std::span<const char*>) {
    const Theme theme;
    set_console_color(theme.prompt_color);
    std::cout << "jshell - An Enhanced C++ Shell for Windows\n";
    set_console_color(theme.default_color);
    std::cout << "Built-in commands:\n";

    // Simply loop through the global 'builtins' table and print the data.
    // No nested loops or complex find operations are needed.
    for (const auto& builtin : builtins) {
        set_console_color(theme.help_command_color);
        std::cout << std::format("  {:10s}", builtin.name);
        set_console_color(theme.default_color);
        std::cout << std::format(" - {}\n", builtin.description);
    }
    return 1;
}
// ========================================================================

int exit_shell(ShellState& state, std::span<const char*>) {
    state.running = false;
    return 1;
}

int pwd(ShellState&, std::span<const char*>) {
    std::cout << fs::current_path().string() << "\n";
    return 1;
}

int env(ShellState&, std::span<const char*>) {
    for(char **env_var = _environ; *env_var; ++env_var) {
        std::cout << *env_var << "\n";
    }
    return 1;
}

int set_env(ShellState&, std::span<const char*> args) {
    if (args.size() < 3) {
        std::cerr << "jshell: Usage: set <NAME> <VALUE>\n";
        return 1;
    }
    if (!SetEnvironmentVariableA(args[1], args[2])) {
        std::cerr << "jshell: set failed\n";
    }
    return 1;
}

int unset_env(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: unset <NAME>\n";
        return 1;
    }
    if (!SetEnvironmentVariableA(args[1], nullptr)) {
        std::cerr << "jshell: unset failed\n";
    }
    return 1;
}

int history(ShellState& state, std::span<const char*>) {
    for (size_t i = 0; i < state.history.size(); ++i) {
        std::cout << std::format("{:5}: {}\n", i + 1, state.history[i]);
    }
    return 1;
}

int source(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: source <script_file>\n";
        return 1;
    }
    std::ifstream file(args[1]);
    if (!file) {
        std::cerr << std::format("jshell: Failed to open script '{}'\n", args[1]);
        return 1;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.starts_with('#')) continue;
        auto commands = split_line(line);
        execute(state, commands);
        if (!state.running) break;
    }
    return 1;
}

int ls(ShellState&, std::span<const char*> args) {
    ParsedArgs parsed = parse_args(args);
    bool long_format = parsed.flags['l'];
    bool show_all = parsed.flags['a'];
    fs::path path = parsed.non_flag_args.empty() ? "." : parsed.non_flag_args[0];
    const Theme theme;

    try {
        if(!fs::exists(path) || !fs::is_directory(path)) {
            throw fs::filesystem_error("Path does not exist or is not a directory", path, std::make_error_code(std::errc::no_such_file_or_directory));
        }

        for (const auto& entry : fs::directory_iterator(path)) {
            std::string filename = entry.path().filename().string();
            if (!show_all && filename.starts_with('.')) continue;

            if (long_format) {
                auto ftime = entry.last_write_time();
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
                char time_buf[20];
                std::strftime(time_buf, sizeof(time_buf), "%b %d %H:%M", std::localtime(&cftime));
                
                std::cout << (entry.is_directory() ? 'd' : '-');
                std::cout << "rwx------ ";
                uintmax_t size = entry.is_directory() ? 0 : fs::file_size(entry);
                std::cout << std::format("{:>10} {} ", size, time_buf);
            }
            
            if (entry.is_directory()) set_console_color(theme.dir_color);
            std::cout << filename << (entry.is_directory() ? "/" : "");
            set_console_color(theme.default_color);
            std::cout << "\n";
        }
    } catch (const fs::filesystem_error& e) {
        set_console_color(theme.error_color);
        std::cerr << std::format("jshell: ls: {}\n", e.what());
        set_console_color(theme.default_color);
    }
    return 1;
}

int cat(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: cat <file>\n";
        return 1;
    }
    std::ifstream file(args[1]);
    if (!file) {
        std::cerr << std::format("jshell: cat: Cannot open file '{}'\n", args[1]);
        return 1;
    }
    std::cout << file.rdbuf();
    return 1;
}

int echo(ShellState&, std::span<const char*> args) {
    for (size_t i = 1; i < args.size(); ++i) {
        std::cout << args[i] << (i == args.size() - 1 ? "" : " ");
    }
    std::cout << "\n";
    return 1;
}

int mkdir(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: mkdir <directory_name>\n";
        return 1;
    }
    try {
        fs::create_directory(args[1]);
    } catch (const fs::filesystem_error& e) {
        std::cerr << std::format("jshell: mkdir: {}\n", e.what());
    }
    return 1;
}

int rm(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: rm <file_name>\n";
        return 1;
    }
    try {
        fs::remove_all(args[1]); // Use remove_all to delete directories too
    } catch (const fs::filesystem_error& e) {
        std::cerr << std::format("jshell: rm: {}\n", e.what());
    }
    return 1;
}

int cls(ShellState&, std::span<const char*>) {
    system("cls");
    return 1;
}

int alias(ShellState& state, std::span<const char*> args) {
    if (args.size() == 1) {
        for (const auto& [name, command] : state.aliases) {
            std::cout << std::format("{}='{}'\n", name, command);
        }
    } else {
        std::string arg_str;
        for(size_t i = 1; i < args.size(); ++i) arg_str += std::string(args[i]) + " ";
        if(!arg_str.empty()) arg_str.pop_back();

        auto pos = arg_str.find('=');
        if (pos == std::string::npos) {
            if (state.aliases.contains(arg_str)) {
                 std::cout << std::format("{}='{}'\n", arg_str, state.aliases[arg_str]);
            } else {
                std::cerr << std::format("jshell: alias not found: {}\n", arg_str);
            }
        } else {
            std::string name = arg_str.substr(0, pos);
            std::string command = arg_str.substr(pos + 1);
            if ((command.starts_with('\'') && command.ends_with('\'')) || (command.starts_with('"') && command.ends_with('"'))) {
                command = command.substr(1, command.length() - 2);
            }
            state.aliases[name] = command;
        }
    }
    return 1;
}

int unalias(ShellState& state, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: unalias <name>\n";
        return 1;
    }
    state.aliases.erase(args[1]);
    return 1;
}

int touch(ShellState&, std::span<const char*> args) {
    if (args.size() < 2) {
        std::cerr << "jshell: Usage: touch <filename>\n";
        return 1;
    }
    std::ofstream file(args[1], std::ios::app);
    if (!file) {
        std::cerr << std::format("jshell: touch: could not create file '{}'\n", args[1]);
    }
    return 1;
}

int execute(ShellState& state, std::vector<Command>& commands) {
    if (commands.empty() || commands[0].args.empty()) {
        return 1;
    }

    if (state.aliases.contains(commands[0].args[0])) {
        std::string alias_cmd_str = state.aliases[commands[0].args[0]];
        std::vector<std::string> original_args(commands[0].args.begin() + 1, commands[0].args.end());
        auto alias_tokens = tokenize(alias_cmd_str);
        commands[0].args = alias_tokens;
        commands[0].args.insert(commands[0].args.end(), original_args.begin(), original_args.end());
    }

    if (commands.size() == 1) {
        for (const auto& builtin : builtins) {
            if (commands[0].args[0] == builtin.name) {
                std::vector<const char*> c_args;
                c_args.reserve(commands[0].args.size());
                for (const auto& s : commands[0].args) c_args.push_back(s.c_str());
                return builtin.func(state, c_args);
            }
        }
    }
    
    int num_pipes = commands.size() - 1;
    if (num_pipes <= 0) {
        return launch(commands[0], INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE);
    }
    
    std::vector<HANDLE> pipe_read(num_pipes);
    std::vector<HANDLE> pipe_write(num_pipes);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

    for (int i = 0; i < num_pipes; ++i) {
        if (!CreatePipe(&pipe_read[i], &pipe_write[i], &sa, 0)) {
            std::cerr << "jshell: CreatePipe failed\n"; return 1;
        }
    }

    for (size_t i = 0; i < commands.size(); ++i) {
        HANDLE hInput = (i == 0) ? INVALID_HANDLE_VALUE : pipe_read[i - 1];
        HANDLE hOutput = (i == commands.size() - 1) ? INVALID_HANDLE_VALUE : pipe_write[i];
        launch(commands[i], hInput, hOutput);
    }
    
    for (auto h : pipe_read) CloseHandle(h);
    for (auto h : pipe_write) CloseHandle(h);
    return 1;
}

void loop() {
    ShellState state;
    const Theme theme;

    fs::path rc_file = fs::path(get_user_profile_directory()) / ".jshellrc";
    if (fs::exists(rc_file)) {
        std::vector<const char*> source_args = {"source", rc_file.string().c_str()};
        source(state, source_args);
    }

    do {
        try {
            std::string line = read_line(state);
            auto commands = split_line(line);
            execute(state, commands);
        } catch (const std::exception& e) {
            set_console_color(theme.error_color);
            std::cerr << std::format("jshell: An error occurred: {}\n", e.what());
            set_console_color(theme.default_color);
        }
    } while (state.running);
}

} // namespace jshell

int main(int argc, char** argv) {
    const jshell::Theme theme;
    
    if (argc > 1) {
        jshell::ShellState state;
        std::vector<const char*> script_args = {"source", argv[1]};
        jshell::source(state, script_args);
        return EXIT_SUCCESS;
    }
    
    try {
        jshell::loop();
    } catch (const std::exception& e) {
        jshell::set_console_color(theme.error_color);
        std::cerr << std::format("jshell: A fatal error occurred: {}\n", e.what());
        jshell::set_console_color(theme.default_color);
        return EXIT_FAILURE;
    }
    
    jshell::set_console_color(theme.default_color);
    std::cout << "Exiting jshell.\n";
    return EXIT_SUCCESS;
}