#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_CMD_LEN 2048
#define MAX_ARGS 512

// Global flag to indicate if the shell is in "foreground-only" mode.
// This flag is controlled by the SIGTSTP signal handler and forces all commands to run in the foreground, even if '&' is specified. (This was HARD)
int fgOnlyMode = 0;

// Stores the exit status or signal termination status of the last foreground process.
// This is used by the "status" built-in command to report the result of the last foreground command execution.
int lastStatus = 0;


// Prototype for the SIGTSTP signal handler.
// This function controls via toggle the "foreground-only" mode of the shell when the user presses Ctrl+Z.
void handle_SIGTSTP(int signo);

// Prototype for the SIGINT signal handler.
// This function allows foreground child processes to be terminated by Ctrl+C, while the parent shell ignores this signal.
void handle_SIGINT(int signo);


// Parses the user's input string into a command and its arguments.
// - input: The raw command line input from the user.
// - args: Array to store individual command arguments.
// - inputFile: Pointer to hold the input file name for redirection (if specified).
// - outputFile: Pointer to hold the output file name for redirection (if specified).
// - background: Pointer to a flag indicating if the command should run in the background.
// Returns 0 for blank lines or comments, and 1 for valid commands.
int parseInput(char *input, char **args, char **inputFile, char **outputFile, int *background);

// Replaces instances of "$$" in a command argument with the process ID of the shell.
// - token: The command argument to be expanded.
// Returns a dynamically allocated string with "$$" replaced by the shell's PID.
char *expandPID(char *token);

// Changes the shell's current working directory.
// - args: Array of arguments; args[1] is the target directory path.
// If no argument is provided, it changes to the HOME directory.
void changeDirectory(char **args);

// Displays the status of the last foreground process.
// Reports the exit value if the process terminated normally, or the signal number if it was killed by a signal.
void displayStatus();

// Executes a command by creating a new process.
// - args: Array of command arguments.
// - inputFile: Input redirection file (if any).
// - outputFile: Output redirection file (if any).
// - background: Flag indicating if the process should run in the background.
// Handles input/output redirection and executes the command using `execvp`.
void executeCommand(char **args, char *inputFile, char *outputFile, int background);

// Checks for completed background processes and cleans up their resources.
// Prints the exit status or termination signal of each completed background process.
void checkBackgroundProcesses();

// Kills all remaining background processes when the shell exits.
// Sends a SIGKILL signal to terminate any lingering child processes.
void killBackgroundProcesses();


int main() {
    // Buffer for storing user input
    char input[MAX_CMD_LEN]; 
    // Array to hold arguments of the command
    char *args[MAX_ARGS]; 
    // Variables for input and output redirection
    char *inputFile = NULL; 
    char *outputFile = NULL; 
    // Flag to indicate if the process should run in the background
    int background = 0; 
    
    // Setting up signal handlers for SIGINT and SIGTSTP
    struct sigaction SIGINT_action = {{0}}, SIGTSTP_action = {{0}}; 

    // Ignore SIGINT (Ctrl+C) in the parent shell to prevent it from terminating
    SIGINT_action.sa_handler = SIG_IGN;
    sigaction(SIGINT, &SIGINT_action, NULL);

    // Handle SIGTSTP (Ctrl+Z) to toggle foreground-only mode
    // SA_RESTART ensures interrupted system calls restart instead of failing
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // Main shell loop
    while (1) {
        // Check if any background processes have completed
        checkBackgroundProcesses();

        // Display the shell prompt
        printf(": ");
        fflush(stdout);

        // Read user input into the buffer
        if (fgets(input, MAX_CMD_LEN, stdin) == NULL) {
            // Handle EOF or read errors gracefully
            clearerr(stdin);
            continue;
        }

        // Remove the trailing newline character from input
        input[strcspn(input, "\n")] = '\0';

        // Parse the input into command arguments and redirection files
        // If the input is a comment or blank line, skip the iteration
        if (!parseInput(input, args, &inputFile, &outputFile, &background)) continue;

        // Check if the command is a built-in command
        if (strcmp(args[0], "exit") == 0) {
            // "exit" command: terminate the shell
            break;
        } else if (strcmp(args[0], "cd") == 0) {
            // "cd" command: change directory
            // Use HOME directory if no argument is provided
            chdir(args[1] ? args[1] : getenv("HOME"));
        } else if (strcmp(args[0], "status") == 0) {
            // "status" command: print the exit status of the last foreground process
            printf("exit value %d\n", WEXITSTATUS(lastStatus));
        } else {
            // Execute an external command
            executeCommand(args, inputFile, outputFile, background);
        }

        // Reset redirection files and background flag for the next command
        inputFile = outputFile = NULL;
        background = 0;
    }

    // Return 0 to indicate successful shell termination
    return 0;
}

int parseInput(char *input, char **args, char **inputFile, char **outputFile, int *background) {
    char *token;         // Token pointer to iterate through input string
    int argCount = 0;    // Counter for the number of arguments parsed

    *background = 0; // Reset background flag for each command
    // WHY: Ensures the background flag does not persist across commands
    // WHAT: Explicitly sets the `background` flag to 0 before processing the current command.

    token = strtok(input, " "); // Split input string by spaces
    // WHY: Breaks the input into tokens (words) for easier parsing of commands, arguments, and redirection operators.

    while (token != NULL && argCount < MAX_ARGS - 1) { 
        // WHY: Continue parsing until there are no more tokens or the maximum argument limit is reached.
        // WHAT: Ensures we process each word in the input.

        // Handle input redirection: '<' is followed by the input file name
        if (strcmp(token, "<") == 0) {
            token = strtok(NULL, " "); // Get the next token, which should be the input file name
            *inputFile = token; // Store the input file name
            // WHY: Assigns the input redirection file name to `inputFile`.
            // WHAT: Enables the program to use this file as standard input for the command.

        // Handle output redirection: '>' is followed by the output file name
        } else if (strcmp(token, ">") == 0) {
            token = strtok(NULL, " "); // Get the next token, which should be the output file name
            *outputFile = token; // Store the output file name
            // WHY: Assigns the output redirection file name to `outputFile`.
            // WHAT: Enables the program to redirect command output to this file.

        // Handle background execution: '&' only if it is the last token
        } else if (strcmp(token, "&") == 0 && strtok(NULL, " ") == NULL) {
            if (fgOnlyMode == 0) *background = 1; 
            // WHY: Sets the `background` flag to 1 if the shell is not in foreground-only mode.
            // WHAT: Indicates the command should run in the background.

        // Treat everything else as a regular argument
        } else {
            char *expanded = expandPID(token); // Expand any occurrences of `$$` in the token
            args[argCount] = expanded; // Store the expanded token in the `args` array
            argCount++; // Increment the argument counter
            // WHY: Processes normal command arguments, including resolving any `$$` into the current process ID.
            // WHAT: Prepares arguments for execution by storing them in the `args` array.
        }

        token = strtok(NULL, " "); // Move to the next token
        // WHY: Continues parsing until all tokens are processed.
        // WHAT: Ensures the loop progresses and doesn't get stuck on the same token.
    }

    args[argCount] = NULL; // Null-terminate the arguments array
    // WHY: Null-termination is required for `execvp` to execute the command properly.
    // WHAT: Marks the end of the command arguments.

    // Check for blank input or comments (starting with `#`)
    return args[0] == NULL || args[0][0] == '#' ? 0 : 1;
    // WHY: Skips processing if the input is empty or starts with a comment.
    // WHAT: Returns `0` to indicate that the shell should skip this iteration.
}

char *expandPID(char *token) {
    // Locate the first occurrence of "$$" in the token
    char *pos = strstr(token, "$$");
    if (pos == NULL) {
        return strdup(token);  // No "$$" found, return a duplicate of the original token
        // WHY: If there is no "$$", the token is returned unchanged.
        // WHAT: Ensures the function works for all tokens, even those without "$$".
    }

    // Get the current process ID
    int pid = getpid();
    char pid_str[16];  // Buffer to hold the string representation of the PID
    snprintf(pid_str, sizeof(pid_str), "%d", pid);
    // WHY: Converts the PID to a string so it can replace "$$".
    // WHAT: Enables substitution of "$$" with the actual process ID.

    // Calculate the length of the new string
    int new_length = strlen(token) - 2 + strlen(pid_str);  // Original length - 2 (for "$$") + PID length
    char *expanded = malloc(new_length + 1);  // Allocate memory for the expanded string
    if (expanded == NULL) {
        perror("malloc");  // Print an error message if memory allocation fails
        exit(1);  // Terminate the program as this is a critical error
        // WHY: Dynamically allocates memory for the expanded string to ensure it fits.
        // WHAT: Ensures the expanded string can store the original token with "$$" replaced by the PID.
    }

    // Construct the expanded string
    snprintf(expanded, new_length + 1, "%.*s%s%s", 
        (int)(pos - token),  // Length of the portion before "$$"
        token,               // Original token up to "$$"
        pid_str,             // Replace "$$" with the PID
        pos + 2);            // Rest of the token after "$$"
    // WHY: Combines the parts of the original string (before and after "$$") with the PID.
    // WHAT: Replaces "$$" with the actual process ID while preserving the rest of the token.

    return expanded;  // Return the expanded string with "$$" replaced by the PID
    // WHY: The caller needs the modified string to use as an argument.
    // WHAT: Allows the shell to process tokens with "$$" as the current process ID.
}

void changeDirectory(char **args) {
    // Check if the user provided a directory argument (args[1])
    if (args[1] == NULL) {
        // No directory specified, change to the user's home directory
        chdir(getenv("HOME"));
        // WHY: If no argument is given, the default behavior of the `cd` command is to navigate to the home directory.
        // WHAT: Uses the `getenv` function to get the "HOME" environment variable, which contains the path to the home directory.
    } else {
        // Attempt to change to the specified directory
        if (chdir(args[1]) != 0) {
            perror("smallsh"); // Print an error message if the directory change fails
            // WHY: `chdir` returns 0 on success and -1 on failure. If it fails, an error message is displayed.
            // WHAT: The `perror` function prints an error message to stderr describing why the change failed.
        }
    }
}

void displayStatus() {
    // Check if the last foreground process exited normally
    if (WIFEXITED(lastStatus)) {
        // If the process exited normally, print its exit value
        printf("exit value %d\n", WEXITSTATUS(lastStatus));
        // WHY: `WIFEXITED` checks if the process terminated normally, and `WEXITSTATUS` extracts its exit code.
        // WHAT: This provides the user with the exit status of the last foreground command.
    } else {
        // If the process was terminated by a signal, print the signal number
        printf("terminated by signal %d\n", WTERMSIG(lastStatus));
        // WHY: `WIFSIGNALED` checks if the process was terminated by a signal, and `WTERMSIG` extracts the signal number.
        // WHAT: This informs the user of the signal (e.g., SIGINT) that caused the process to terminate abnormally.
    }

    // Ensure the output is immediately displayed on the screen
    fflush(stdout);
    // WHY: `fflush(stdout)` forces the output buffer to be flushed to the screen.
    // WHAT: This ensures that the status message is printed promptly, even if output buffering is enabled.
}

void executeCommand(char **args, char *inputFile, char *outputFile, int background) {
    // Force foreground execution if in foreground-only mode
    if (fgOnlyMode == 1) {
        background = 0;  // Ignore background requests when foreground-only mode is active
        // WHY: Foreground-only mode disables background execution by ignoring the '&' operator.
        // WHAT: Ensures all commands run in the foreground when this mode is enabled.
    }

    pid_t spawnpid = fork();  // Create a child process to execute the command

    if (spawnpid == -1) {
        perror("fork");  // Print error if fork fails
        exit(1);  // Exit the program as we cannot continue without forking
        // WHY: Forking creates a new process to run the command; failure means no process is available.
        // WHAT: The shell cannot execute further commands if forking fails.
    } else if (spawnpid == 0) {  // Child process block
        // Child process: set up signal handling for foreground and background
        struct sigaction SIGINT_action = {{0}};
        sigemptyset(&SIGINT_action.sa_mask);

        // Background processes ignore SIGINT; foreground processes handle it normally
        SIGINT_action.sa_handler = (background) ? SIG_IGN : SIG_DFL;
        sigaction(SIGINT, &SIGINT_action, NULL);
        // WHY: SIGINT (Ctrl+C) should not terminate background processes.
        // WHAT: Foreground processes can be interrupted by the user; background processes cannot.

        // Handle input redirection
        if (inputFile != NULL) {  // If input redirection is specified
            int inputFD = open(inputFile, O_RDONLY);  // Open the input file for reading
            if (inputFD == -1) {
                perror("cannot open input file");  // Print error if file cannot be opened
                exit(1);  // Exit with failure
            }
            if (dup2(inputFD, 0) == -1) {  // Redirect standard input to the file
                perror("dup2 input");  // Print error if redirection fails
                close(inputFD);
                exit(1);  // Exit with failure
            }
            close(inputFD);  // Close the file descriptor after redirection
            // WHY: Redirects standard input from the specified file, enabling input redirection.
            // WHAT: Allows the command to read input from a file instead of the terminal.
        } else if (background) {  // For background processes without input redirection
            int devNull = open("/dev/null", O_RDONLY);  // Redirect input to /dev/null
            if (devNull == -1 || dup2(devNull, 0) == -1) {
                perror("dup2 input to /dev/null");
                close(devNull);
                exit(1);
            }
            close(devNull);
            // WHY: Background processes should not wait for user input.
            // WHAT: Redirects input to /dev/null to avoid waiting for input.
        }

        // Handle output redirection
        if (outputFile != NULL) {  // If output redirection is specified
            int outputFD = open(outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);  // Open or create the output file
            if (outputFD == -1) {
                perror("cannot open output file");  // Print error if file cannot be opened
                exit(1);  // Exit with failure
            }
            if (dup2(outputFD, 1) == -1) {  // Redirect standard output to the file
                perror("dup2 output");  // Print error if redirection fails
                close(outputFD);
                exit(1);  // Exit with failure
            }
            close(outputFD);  // Close the file descriptor after redirection
            // WHY: Redirects standard output to the specified file, enabling output redirection.
            // WHAT: Allows the command to write output to a file instead of the terminal.
        } else if (background) {  // For background processes without output redirection
            int devNull = open("/dev/null", O_WRONLY);  // Redirect output to /dev/null
            if (devNull == -1 || dup2(devNull, 1) == -1) {
                perror("dup2 output to /dev/null");
                close(devNull);
                exit(1);
            }
            close(devNull);
            // WHY: Prevents background processes from cluttering the terminal with output.
            // WHAT: Redirects output to /dev/null to suppress it.
        }

        // Execute the command
        execvp(args[0], args);  // Replace the child process with the specified command
        perror(args[0]);  // Print error if execvp fails
        exit(1);  // Exit with failure if command execution fails
        // WHY: execvp runs the specified command, replacing the child process image.
        // WHAT: If execvp returns, it means execution failed, so the child exits with an error.
    } else {  // Parent process block
        if (background && fgOnlyMode == 0) {  // For background processes not in foreground-only mode
            printf("background pid is %d\n", spawnpid);  // Print the PID of the background process
            fflush(stdout);
            // WHY: Notifies the user that a command is running in the background.
            // WHAT: Provides feedback about the background process PID.
        } else {  // For foreground processes
            spawnpid = waitpid(spawnpid, &lastStatus, 0);  // Wait for the process to finish
            if (WIFSIGNALED(lastStatus)) {  // Check if process terminated due to a signal
                printf("terminated by signal %d\n", WTERMSIG(lastStatus));  // Print the signal number
                fflush(stdout);
                // WHY: Informs the user if a process was terminated abnormally by a signal.
                // WHAT: Provides feedback about unexpected terminations.
            }
        }
    }
}

void handle_SIGTSTP(int signo) {
    // Check if foreground-only mode is currently disabled
    if (fgOnlyMode == 0) {
        fgOnlyMode = 1;  // Enable foreground-only mode
        char *message = "\nEntering foreground-only mode (& is now ignored)\n";
        // Write the message to the standard output (terminal)
        if (write(STDOUT_FILENO, message, strlen(message)) == -1) {
            perror("write");  // Print an error if the write operation fails
        }
        // WHY: Enables foreground-only mode, which forces all commands to run in the foreground.
        // WHAT: Notifies the user that foreground-only mode has been activated.
    } else {
        fgOnlyMode = 0;  // Disable foreground-only mode
        char *message = "\nExiting foreground-only mode\n";
        // Write the message to the standard output (terminal)
        if (write(STDOUT_FILENO, message, strlen(message)) == -1) {
            perror("write");  // Print an error if the write operation fails
        }
        // WHY: Disables foreground-only mode, allowing background execution with '&'.
        // WHAT: Notifies the user that foreground-only mode has been deactivated.
    }
}

void checkBackgroundProcesses() {
    int childStatus;  // Variable to store the status of a child process
    pid_t pid;        // Variable to store the PID of a finished child process

    // Loop to reap all finished background processes
    while ((pid = waitpid(-1, &childStatus, WNOHANG)) > 0) {
        // WHY: `waitpid` is called with `-1` to check all child processes,
        // and `WNOHANG` ensures it doesn't block if no processes have finished.
        // WHAT: This loop retrieves the status of all completed background processes.

        // Only print status if a background process has finished
        if (!fgOnlyMode) {  // Print background completion status only if not in fgOnlyMode
            printf("background pid %d is done: ", pid);
            // WHY: In foreground-only mode, background processes aren't relevant, so we skip reporting them.

            // Check if the process terminated normally
            if (WIFEXITED(childStatus)) {
                printf("exit value %d\n", WEXITSTATUS(childStatus));
                // WHY: If the process exited normally, we report its exit value.
                // WHAT: `WEXITSTATUS(childStatus)` extracts the exit code from `childStatus`.
            } else if (WIFSIGNALED(childStatus)) {
                printf("terminated by signal %d\n", WTERMSIG(childStatus));
                // WHY: If the process was terminated by a signal, we report the signal number.
                // WHAT: `WTERMSIG(childStatus)` extracts the signal that caused the termination.
            }
            fflush(stdout);  // Ensure all output is immediately displayed
            // WHY: We flush the output to prevent delays in displaying the status.
        }
    }
}

void killBackgroundProcesses() {
    pid_t pid; // Variable to store the PID of any child process

    // Loop through all background processes and terminate them
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        // WHY: `waitpid` with `-1` checks all child processes, and `WNOHANG` ensures
        // it doesn't block if no processes are ready. This retrieves PIDs of all
        // background processes still running.
        // WHAT: The loop finds all child processes that haven't been reaped yet.

        kill(pid, SIGKILL);
        // WHY: Send the `SIGKILL` signal to forcibly terminate the process with the given PID.
        // This ensures no lingering background processes remain when the shell exits.
        // WHAT: `SIGKILL` is a non-catchable signal that immediately stops the process.
    }
}