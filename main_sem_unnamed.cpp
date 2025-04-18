#include <iostream>
#include <vector>
#include <random>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <cstring>
#include <cmath>
#include <signal.h>
#include <string>

#define SHM_NAME "/main_shm"

struct TournamentState {
    int players_moves[100];  
    bool is_in_game[100];            
    int opponent[100];           
    int tournament_winner;                   
    int round_winners[100];       
    int winners_count;         
    int current_round;             
    int total_players;            
    bool is_finished;          
    sem_t main_sem;
    sem_t move_made_sem;
    sem_t player_sems[100];
};

std::vector<pid_t> playerPids;
int n, shm_fd;
TournamentState* tournamentState;

std::string moves[3] = {"камень", "ножницы", "бумага"};

int referee(int move1, int move2) {
    if (move1 == move2) {
        return 0;
    } else if (move1 == 0 && move2 == 1 || move1 == 1 && move2 == 2 || move1 == 2 && move2 == 0) {
        return 1;
    }
    return 2;
}

void playerProcess(int id, TournamentState* tournamentState) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);
        
    while (true) {
        if (sem_wait(&tournamentState->player_sems[id - 1]) == -1) {
            perror("sem_wait on player semaphore");
            exit(1);
        }
        
        if (sem_wait(&tournamentState->main_sem) == -1) {
            perror("sem_wait on main semaphore");
            exit(1);
        }
        
        if (tournamentState->is_finished || !tournamentState->is_in_game[id - 1]) {
            sem_post(&tournamentState->main_sem);
            break;
        }
        
        int move = distrib(gen);
        tournamentState->players_moves[id - 1] = move;
        
        sem_post(&tournamentState->main_sem);
        sem_post(&tournamentState->move_made_sem);
    }
    
    exit(0);
}

void handle_sigint(int sig) {
    std::cout << "Получен SIGINT, завершаю турнир и очищаю ресурсы..." << std::endl;

    for (int i = 0; i < n; i++) {
        sem_post(&tournamentState->player_sems[i]);
    }
    
    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }
    
    for (int i = 0; i < n; i++) {
        sem_destroy(&tournamentState->player_sems[i]);
    }
    
    sem_destroy(&tournamentState->main_sem);
    sem_destroy(&tournamentState->move_made_sem);
    
    munmap(tournamentState, sizeof(TournamentState));
    close(shm_fd);
    shm_unlink(SHM_NAME);
    exit(0);
}

int main() {
    struct sigaction sa{.__sigaction_handler = handle_sigint, .sa_flags = SA_SIGINFO};
    sigaction(SIGINT, &sa, NULL);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(2, 101);
    n = distrib(gen);

    std::cout << "Количество игроков в турнире: " << n << std::endl;
    
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }
    
    if (ftruncate(shm_fd, sizeof(TournamentState)) == -1) {
        perror("ftruncate");
        return 1;
    }
    
    tournamentState = (TournamentState*) mmap(NULL, sizeof(TournamentState), 
                                               PROT_READ | PROT_WRITE, MAP_SHARED, 
                                               shm_fd, 0);
    if (tournamentState == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    
    memset(tournamentState, 0, sizeof(TournamentState));

    // Инициализация семафоров
    if (sem_init(&tournamentState->main_sem, 1, 1) == -1) {
        perror("sem_init (main_sem)");
        return 1;
    }
    
    if (sem_init(&tournamentState->move_made_sem, 1, 0) == -1) {
        perror("sem_init (move_made_sem)");
        return 1;
    }
    
    for (int i = 0; i < n; i++) {
        if (sem_init(&tournamentState->player_sems[i], 1, 0) == -1) {
            perror("sem_init (player_sem)");
            return 1;
        }
    }

    tournamentState->total_players = n;
    tournamentState->tournament_winner = -1;
    tournamentState->is_finished = false;
    tournamentState->current_round = 1;
    
    for (int i = 0; i < n; i++) {
        tournamentState->is_in_game[i] = true;
        tournamentState->opponent[i] = -1;
    }
    
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return 1;
        }
        
        if (pid == 0) {
            playerProcess(i + 1, tournamentState);
            exit(0);
        }
        
        playerPids.push_back(pid);
    }
    
    int numRounds = static_cast<int>(std::ceil(std::log2(n)));
    
    std::vector<int> activeStudents;
    for (int i = 0; i < n; i++) {
        activeStudents.push_back(i);
    }
    
    for (int round = 1; round <= numRounds; round++) {
        std::cout << "\nРаунд " << round << std::endl;

        tournamentState->winners_count = 0;
        
        sem_wait(&tournamentState->main_sem);
        tournamentState->current_round = round;
        sem_post(&tournamentState->main_sem);
        
        int match_count = activeStudents.size() / 2;

        if (activeStudents.size() - match_count * 2 == 1) {
            int student_idx = activeStudents[activeStudents.size() - 1];

            std::cout << "Студент " << student_idx + 1 << " проходит в следующий раунд" << std::endl;

            sem_wait(&tournamentState->main_sem);
            tournamentState->round_winners[tournamentState->winners_count++] = student_idx;
            sem_post(&tournamentState->main_sem);
        }
        
        for (int match = 0; match < match_count; match++) {
            int player1 = activeStudents[match * 2];
            int player2 = activeStudents[match * 2 + 1];
            
            std::cout << "Матч между " << player1 + 1 << " и " << player2 + 1 << std::endl;
            
            sem_wait(&tournamentState->main_sem);
            tournamentState->opponent[player1] = player2;
            tournamentState->opponent[player2] = player1;
            sem_post(&tournamentState->main_sem);
            
            sem_post(&tournamentState->player_sems[player1]);
            sem_post(&tournamentState->player_sems[player2]);
            
            bool matchDecided = false;
            while (!matchDecided) {
                sem_wait(&tournamentState->move_made_sem);
                sem_wait(&tournamentState->move_made_sem);
                
                sem_wait(&tournamentState->main_sem);
                int choice1 = tournamentState->players_moves[player1];
                int choice2 = tournamentState->players_moves[player2];
                sem_post(&tournamentState->main_sem);
                
                std::cout << "   Игрок " << player1 + 1 << " выбрал " << moves[choice1] << std::endl;
                std::cout << "   Игрок " << player2 + 1 << " выбрал " << moves[choice2] << std::endl;
                
                int result = referee(choice1, choice2);
                if (result == 1) {
                    std::cout << "  Игрок " << player1 + 1 << " победил" << std::endl;
                    
                    sem_wait(&tournamentState->main_sem);
                    tournamentState->is_in_game[player2] = false;
                    tournamentState->round_winners[tournamentState->winners_count++] = player1;
                    sem_post(&tournamentState->main_sem);
                    
                    matchDecided = true;
                } else if (result == 2) {
                    std::cout << "  Игрок " << player2 + 1 << " победил" << std::endl;
                    
                    sem_wait(&tournamentState->main_sem);
                    tournamentState->is_in_game[player1] = false;
                    tournamentState->round_winners[tournamentState->winners_count++] = player2;
                    sem_post(&tournamentState->main_sem);
                    
                    matchDecided = true;
                } else {
                    std::cout << "  Ничья, матч переигрывается... " << std::endl;
                    
                    sem_post(&tournamentState->player_sems[player1]);
                    sem_post(&tournamentState->player_sems[player2]);
                }
            }
        }
        
        activeStudents.clear();
        sem_wait(&tournamentState->main_sem);
        for (int i = 0; i < tournamentState->winners_count; i++) {
            activeStudents.push_back(tournamentState->round_winners[i]);
        }
        sem_post(&tournamentState->main_sem);
        
        if (activeStudents.size() == 1) {
            sem_wait(&tournamentState->main_sem);
            tournamentState->tournament_winner = activeStudents[0] + 1;
            tournamentState->is_finished = true;
            sem_post(&tournamentState->main_sem);
            
            std::cout << "\nТурнир закончен, выиграл игрок под номером: " << tournamentState->tournament_winner << std::endl;
            break;
        }
    }
    
    for (int i = 0; i < n; i++) {
        sem_post(&tournamentState->player_sems[i]);
    }
    
    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }
    
    for (int i = 0; i < n; i++) {
        sem_destroy(&tournamentState->player_sems[i]);
    }
    
    sem_destroy(&tournamentState->main_sem);
    sem_destroy(&tournamentState->move_made_sem);
    
    munmap(tournamentState, sizeof(TournamentState));
    close(shm_fd);
    shm_unlink(SHM_NAME);
    
    return 0;
} 