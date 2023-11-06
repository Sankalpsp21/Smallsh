// If you are not compiling with the gcc option --std=gnu99, then
// uncomment the following line or you might get a compiler warning
//#define _GNU_SOURCE

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_ARGS 512
pid_t currentForegroundChild = -21;
int lastExitMethod = -21;
int SIGTSTP_toggler = 0;
int backgroundChildrenRunning = 0;

void mainLoop();


// -------------------------------------BUILT IN COMMANDS------------------------------------

//Cannot be run in the background
void changeDirectory(char** argv, int argc){

    //If the last argument was an &, ignore it
    if((strcmp(argv[argc - 1], "&") == 0)){
        argv[argc - 1] = NULL; //Removing the ampersand from the argument list
        argc--;
    }

    //If no arguments were given, change to the home directory
    if(argc == 1){

        //Getting home directory
        char* homeDir = getenv("HOME");
        chdir(homeDir);

    //A path was given
    }else{

        //Creating buffer to hold our cwd name
        char cwd[1000];             
        char slash[] = "/";
        memset(cwd, '\0', 1000);

        //If the path is a relative path...
        if(argv[1][0] != '/'){

            //Getting the cwd and appending a slash and the argument
            getcwd(cwd, sizeof(cwd));   
            strcat(cwd, slash);
            strcat(cwd, argv[1]);             


        //If the path is an absolute path
        }else{
            strcpy(cwd, argv[1]);
        }

        //Changing the cwd to the new directory
        chdir(cwd);

        // printf("%s\n", cwd);
    }
}

// Kills any other processes or jobs the shell has started before it terminates itself.
void myExit(){

    //Getting the shell's group pid. This includes the process and all it's child processes
    pid_t shell_pid = getpid();
    pid_t group_pid = getpgid(shell_pid);

    kill(-group_pid, SIGTERM);
    exit(0);
}

// Prints out either the exit status or the terminating signal of the last foreground process.
void status(){

    //If nothing was run yet just return 0.
    if(lastExitMethod == -21){
        printf("exit value 0\n"); fflush(stdout);

    //if the child was terminated normally, get the exit value
    }else if(WIFEXITED(lastExitMethod)){
        printf("exit value %d\n", WEXITSTATUS(lastExitMethod)); fflush(stdout);

    //The child was terminated by a signal, get the signal value
    }else {
        printf("Terminated by signal. Exit value %d\n", WTERMSIG(lastExitMethod)); fflush(stdout);
    }
}
// ------------------------------------------------------------------------------------------


//Sets up I/O redirection using dup2()
// If it cannot open a file for reading, return 1
// If it cannot open an output file, return 1
// If everything went well, return 0
// If it is a backrgound command and no redirection is specified, input and output is /dev/null
int ioRedirection(char** argv, int argc, int background){
    int outFile = -5;
    int inFile = -5;
    
    int indexesToRemove[MAX_ARGS];
    int numIoChars = 0;

    int outDevNull = open("/dev/null", O_WRONLY);
    int inDevNull = open("/dev/null", O_RDONLY);

    //If there are I/O redirection chars in the arg list, open the files for input and output
    for(int i = 1; i < argc - 1; i++){
        // printf("%d\n", i);

        //If it's redirecting output, open the output file
        if (strcmp(argv[i], ">") == 0){
            outFile = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(outFile == -1){
                return 1;
            }

            //Removing I/O characters from the argument list
            indexesToRemove[numIoChars++] = i;
            indexesToRemove[numIoChars++] = i + 1;
        }
        
        //If it's redirecting input, open the input file
        else if (strcmp(argv[i], "<") == 0){
            inFile = open(argv[i + 1], O_RDONLY);
            if(inFile == -1){
                return 1;
            }

            //Removing I/O characters from the argument list
            indexesToRemove[numIoChars++] = i;
            indexesToRemove[numIoChars++] = i + 1;
        }
    }

    if (outFile == -5 && background == 1){
        //redirect stdout to /dev/null
        dup2(outDevNull, 1);
    }

    if (inFile == -5 && background == 1){
        //redirect stdin to /dev/null
        dup2(inDevNull, 0);
    }

    if(outFile != -5){
        //redirect stdout to outFile
        // printf("Out is redirected\n"); fflush(stdout);

        dup2(outFile, 1);
    }

    if(inFile != -5){
        //redirect stdin to inFile
        // printf("In is redirected\n");
        dup2(inFile, 0);
    }

    //Removing all arguments associate with I/O redirection
    for(int i = 0; i < numIoChars; i++){
        argv[indexesToRemove[i]] = NULL;
    }

    argc -= numIoChars;

    return 0;
}

// Returns 1 if the command will run in the background, 0 is not
int isBackground(char** argv, int argc){
    int background = 0;

    //If the last argument was an "&"...
    if((strcmp(argv[argc - 1], "&") == 0)){

        //and SIGTSTP_toggler is 0 (CTRL+Z) then run it in the background
        if(!SIGTSTP_toggler){
            background = 1;
        }

        //Regardless of SIGTSTP_toggler, we remove the ampersand
        argv[argc - 1] = NULL; 
        argc--;
    }

    return background;
}

//Resets the CTRL+C for the child process
void setupChildSignals(){
    // Child process
    struct sigaction SIGINT_action = {0};

    SIGINT_action.sa_handler = SIG_DFL;
    sigemptyset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;

    sigaction(SIGINT, &SIGINT_action, NULL);
}

//Checks to see if the last exit method signifies being terminated by a signal. 
// If so, print out the signal
void checkForSIGINT(){
    if(WIFSIGNALED(lastExitMethod)){
        printf("\nterminated by signal %d\n", WTERMSIG(lastExitMethod)); fflush(stdout);
    }
}

//Running a non-built-in command
void runForkExec(char** argv, int argc){
    int childExitMethod = -21;
    pid_t spawnpid = -21;
    int background = 0;
    int exitStatus = 0;

    // If command has '&' as the end and ctrl+z hasn't disabled bg processes, background is set to 1;
    background = isBackground(argv, argc);

    //Creating child process
    spawnpid = fork();

    switch (spawnpid){
        case -1:
            perror("Hull Breach!"); fflush(stdout);
            exit(1);
            break;
        
        case 0:
            if(!background){
                //Start listening for CTRL+C
                currentForegroundChild = getpid();
                setupChildSignals();
            }
        
            //Sets up any I/O redirection 
            exitStatus = ioRedirection(argv, argc, background);

            //If the redirection is invalid, exit
            if(exitStatus){
                fprintf(stderr, "Error opening file for input/output redirection\n");
                exit(1);
            }

            //If the background option is set, add an & to the end of the argument list
            if(background){
                argv[argc] = malloc(strlen("&") + 1);
                argv[argc] = "&";
                argc++;
            }

            // Searching the PATH environmnet variable for the executable given in argv[0]
            // Child running here should terminate with a CTRL + C
            execvp(argv[0], argv);

            //THIS WILL ONLY RUN IF EXECVP FAILS----------------------------
            //If the execvp command fails, the command wasn't found in PATH. 
            fprintf(stderr, "Command not recognized\n");
            exit(1);
            break;
            //--------------------------------------------------------------

        default:
            // printf("I am the parent!\n"); fflush(stdout);
            //If it's not a background process, wait for it normally
            if(!background){
                currentForegroundChild = spawnpid;

                spawnpid = waitpid(spawnpid, &childExitMethod, 0);
                lastExitMethod = childExitMethod; //Saving the last exit method for status()
                checkForSIGINT();

            }else{
                //If it is a background process, increment counter so that the main loop checks for child processes
                printf("Background pid is %d\n", spawnpid);
                backgroundChildrenRunning++;
            }

            break;
    }
}

//Expands all instances of $$ in the argument list to the smallsh pid
void expandVariable(char** argv, int argc){

    char* flag = "$$";
    char pidStr[16];
    sprintf(pidStr, "%d", getpid());

    for (int i = 0; i < argc; i++){
        char* flagPointer = argv[i];

        //While $$ is found within the argument, flagPointer will hold the location of $$
        while((flagPointer = strstr(flagPointer, flag)) != NULL){
            int indexOfFlag = flagPointer - argv[i]; //Get the index of the substring

            //Shifts the everthing after the $$ to make room for the pidStr
            memmove(flagPointer + strlen(pidStr), flagPointer + strlen(flag), strlen(flagPointer + strlen(flag)) + 1);

            //Copying the pidStr where the $$ used to be within the string
            memcpy(flagPointer, pidStr, strlen(pidStr));
            flagPointer += strlen(pidStr);
        }
    }
}

//This directs the program to the appropriate function based on the command
void delegate(char** argv, int argc){

    //If the line begins with '#' or is blank, reprompt user
    if(argv[0][0] == '#' || argv[0][0] == '\n'){
        return;
    }

    //Replaces the \n character in the last argument with null terminator
    size_t len = strlen(argv[argc - 1]);
    if (len > 0 && argv[argc - 1][len - 1] == '\n') {
        argv[argc - 1][len - 1] = '\0';
    }

    //Expands all instances of $$ to the pid of the smallsh itself
    expandVariable(argv, argc);

    if(strcmp(argv[0], "exit") == 0){
        //Exit the program
        myExit();

    }else if(strcmp(argv[0], "cd") == 0){
        //do cd
        changeDirectory(argv, argc);

    }else if(strcmp(argv[0], "status") == 0){
        //do status
        // printf("status!\n");
        status();

    }else{
        //Not a built-in command. Send it off to OS
        // printf("sending to fork and exec!\n");
        runForkExec(argv, argc);
    }
}

void checkInOnChildren(){
    //If there are any background processes running, check in on them
    if(backgroundChildrenRunning){

        //Checks if any processes have completed
        pid_t completedChild = waitpid(-1, &lastExitMethod, WNOHANG);

        //If a process has completed, print out its pid, exit method and status
        if(completedChild > 0){
            printf("Background pid %d is done: ", completedChild); fflush(stdout);

            if(WIFEXITED(lastExitMethod)){
                printf("exit value %d\n", WEXITSTATUS(lastExitMethod)); fflush(stdout);
            }else{
                printf("terminated by signal %d\n", WTERMSIG(lastExitMethod)); fflush(stdout);
            }
        }
    }
}

//Signal Handler for SIGTSTP (CTRL + Z)
void catchSIGTSTP(int signo){

    //Toggles the setting that CTRL+Z creates in the shell

    if (SIGTSTP_toggler == 0){
        SIGTSTP_toggler = 1;
        write(STDOUT_FILENO, "\nEntering foreground-only mode (& is now ignored)\n: ", 54);
        //Make it so commands CAN'T be run in the background

    } else {
        SIGTSTP_toggler = 0;
        write(STDOUT_FILENO, "\nExiting foreground-only mode\n", 32);
        //Make it so commands CAN be run in the background
    }

}

//Sets up signal handlers for SIGTSTP (CTRL + Z) and SIGINT (CTRL + C)
void setUpSignalHandlers(){
    struct sigaction SIGTSTP_action = {0}, ignore_action = {0};

    SIGTSTP_action.sa_handler = catchSIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;


    ignore_action.sa_handler = SIG_IGN;

    //Main shell will handle Ctrl+Z with catchSIGTSTP()
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    //Main shell will ignore Ctrl+C 
    sigaction(SIGINT, &ignore_action, NULL);
}

void mainLoop(){

    while(1){

        //Checking all child processes for completion
        checkInOnChildren();

        //Printing out default prompt
        printf(": "); fflush(stdout);

        //Initializing the input char array
        char* input = NULL;
        char** argv = malloc(MAX_ARGS * sizeof(char*));
        size_t size = 0;
        int counter = 0;

        getline(&input, &size, stdin);

        //Parsing the input into a char array
        char* token = strtok(input, " ");

        while (token != NULL && counter < MAX_ARGS) {
            //For every space-seperated part of the command,
            //allocate memory and copy over the part into argv
            argv[counter] = malloc(strlen(token) + 1);
            strcpy(argv[counter], token);

            //Increment the counter and get next part
            counter++;
            token = strtok(NULL, " ");
        }

        //Setting the last argument as NULL
        argv[counter] = NULL;

        // printf("%s\n", argv[0]);

        //Sending the input off to be processed
        delegate(argv, counter);

        // printf("%s\n", inputArray[0]); fflush(stdout);

        //Freeing everything
        // for (int i = 0; i < counter; i++){
        //     free(inputArray[i]);
        // }

    }

}


int main(){

    setUpSignalHandlers();
    mainLoop();

    /*
    *   Compile the program as follows:
    *       gcc --std=gnu99 -o smallsh smallsh.c
    * 
    *   To KILL
    *       pkill smallsh 2> /dev/null
    */
    return 0;
}
