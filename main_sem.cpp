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
#define MAIN_SEM "/main_sem"
#define PLAYER_SEM "/player_"
#define MOVE_MADE_SEM "/move_made"


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
};

std::vector<sem_t*> playerSems;
std::vector<pid_t> playerPids;
int n, shm_fd;
TournamentState* tournamentState;
sem_t* mainSem;
sem_t* moveMadeSem;

std::string moves[3] = {"камень", "ножницы", "бумага"};


int referee(int move1, int move2) {
    if (move1 == move2) {
        return 0;
    } else if (move1 == 0 && move2 == 1 || move1 == 1 && move2 == 2 || move1 == 2 && move2 == 0) {
        return 1;
    }
    return 2;
};

void playerProcess(int id, TournamentState* tournamentState, sem_t* mainSem, sem_t* playerSem, sem_t* moveMadeSem) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 2);
        
    while (true) {
        if (sem_wait(playerSem) == -1) {
            perror("sem_wait on player semaphore");
            exit(1);
        }
        
        if (sem_wait(mainSem) == -1) {
            perror("sem_wait on main semaphore");
            exit(1);
        }
        
        if (tournamentState->is_finished || !tournamentState->is_in_game[id - 1]) {
            sem_post(mainSem);
            break;
        }
        
        int move = distrib(gen);
        tournamentState->players_moves[id - 1] = move;
        
        sem_post(mainSem);
        sem_post(moveMadeSem);
    }
    
    exit(0);
}

void handle_sigint(int sig) {
    std::cout << "Получен SIGINT, завершаю турнир и очищаю ресурсы..." << std::endl;

    for (int i = 0; i < playerSems.size(); i++) {
        sem_post(playerSems[i]);
    }
    
    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }
    
    for (sem_t* sem : playerSems) {
        sem_close(sem);
    }
    
    if (mainSem) sem_close(mainSem);
    if (moveMadeSem) sem_close(moveMadeSem);
    
    for (int i = 0; i < playerSems.size(); i++) {
        std::string semName = PLAYER_SEM + std::to_string(i + 1);
        if (sem_unlink(semName.c_str()) == -1) {
            perror("sem_unlink");
        }
    }
    
    if (sem_unlink(MAIN_SEM) == -1) {
        perror("sem_unlink");
    }
    if (sem_unlink(MOVE_MADE_SEM) == -1) {
        perror("sem_unlink");
    }

    
    munmap(tournamentState, sizeof(TournamentState));
    close(shm_fd);
    shm_unlink(SHM_NAME);
    exit(0);
};

int main() {
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

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

    tournamentState->total_players = n;
    tournamentState->tournament_winner = -1;
    tournamentState->is_finished = false;
    tournamentState->current_round = 1;
    
    for (int i = 0; i < n; i++) {
        tournamentState->is_in_game[i] = true;
        tournamentState->opponent[i] = -1;
    }
    
    mainSem = sem_open(MAIN_SEM, O_CREAT, 0666, 1);
    if (mainSem == SEM_FAILED) {
        perror("sem_open (mainSem)");
        return 1;
    }
    
    moveMadeSem = sem_open(MOVE_MADE_SEM, O_CREAT, 0666, 0);
    if (moveMadeSem == SEM_FAILED) {
        perror("sem_open (moveMadeSem)");
        return 1;
    }

    
    for (int i = 0; i < n; i++) {
        std::string semName = PLAYER_SEM + std::to_string(i + 1);
        sem_t* playerSem = sem_open(semName.c_str(), O_CREAT, 0666, 0);
        if (playerSem == SEM_FAILED) {
            perror("sem_open (playerSem)");
            return 1;
        }
        playerSems.push_back(playerSem);
    }
    
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return 1;
        }
        
        if (pid == 0) {
            playerProcess(i + 1, tournamentState, mainSem, playerSems[i], moveMadeSem);
            exit(0);
        }
        
        playerPids.push_back(pid);
    }
    
    int numRounds = static_cast<int>(std::ceil(std::log2(n))); // Кол-во раундов
    
    std::vector<int> activeStudents;
    for (int i = 0; i < n; i++) {
        activeStudents.push_back(i);
    }
    
    for (int round = 1; round <= numRounds; round++) {
        std::cout << "\nРаунд " << round << std::endl;

        tournamentState->winners_count = 0;
        
        // Блокируем основной семафор
        sem_wait(mainSem);
        tournamentState->current_round = round;
        sem_post(mainSem);
        
        int match_count = activeStudents.size() / 2;

        if (activeStudents.size() - match_count * 2 == 1) {
            int student_idx = activeStudents[activeStudents.size() - 1];

            std::cout << "Студент " << student_idx + 1 << " проходит в следующий раунд" << std::endl;

            sem_wait(mainSem);
            tournamentState->round_winners[tournamentState->winners_count++] = student_idx;
            sem_post(mainSem);
        }
        
        for (int match = 0; match < match_count; match++) {
            int player1 = activeStudents[match * 2];
            int player2 = activeStudents[match * 2 + 1];
            
            std::cout << "Матч между " << player1 + 1 << " и " << player2 + 1 << std::endl;
            
            sem_wait(mainSem);
            tournamentState->opponent[player1] = player2;
            tournamentState->opponent[player2] = player1;
            sem_post(mainSem);
            
            // триггер для игроков сделать ход
            sem_post(playerSems[player1]);
            sem_post(playerSems[player2]);
            
            bool matchDecided = false;
            while (!matchDecided) {
                // ждем пока два игрока сделают ход
                sem_wait(moveMadeSem);
                sem_wait(moveMadeSem);
                
                sem_wait(mainSem);
                int choice1 = tournamentState->players_moves[player1];
                int choice2 = tournamentState->players_moves[player2];
                sem_post(mainSem);

                std::cout << "   Игрок " << player1 + 1 << " выбрал " << moves[choice1] << std::endl;
                std::cout << "   Игрок " << player2 + 1 << " выбрал " << moves[choice2] << std::endl;
                
                int result = referee(choice1, choice2);
                if (result == 1) {
                    
                    std::cout << "  Игрок " << player1 + 1 << " победил" << std::endl;
                    
                    sem_wait(mainSem);
                    tournamentState->is_in_game[player2] = false;
                    tournamentState->round_winners[tournamentState->winners_count++] = player1;
                    sem_post(mainSem);
                    
                    matchDecided = true;
                } else if (result == 2) {
                    std::cout << "  Игрок " << player2 + 1 << " победил" << std::endl;
                    
                    sem_wait(mainSem);
                    tournamentState->is_in_game[player1] = false;
                    tournamentState->round_winners[tournamentState->winners_count++] = player2;
                    sem_post(mainSem);
                    
                    matchDecided = true;
                } else {
                    std::cout << "  Ничья, матч переигрывается... " << std::endl;
                    
                    // триггер для переигровки матча
                    sem_post(playerSems[player1]);
                    sem_post(playerSems[player2]);
                }
            }
        }
        
        // очистка списка активных игроков для следующего раунда
        activeStudents.clear();
        sem_wait(mainSem);
        for (int i = 0; i < tournamentState->winners_count; i++) {
            activeStudents.push_back(tournamentState->round_winners[i]);
        }
        sem_post(mainSem);
        
        // проверка на наличие победителя
        if (activeStudents.size() == 1) {
            sem_wait(mainSem);
            tournamentState->tournament_winner = activeStudents[0] + 1;
            tournamentState->is_finished = true;
            sem_post(mainSem);
            
            std::cout << "\nТурнир закончен, выиграл игрок под номером: " << tournamentState->tournament_winner << std::endl;
            break;
        }
    }
    
    // освобождение игроков, чтобы они могли завершиться
    for (int i = 0; i < n; i++) {
        sem_post(playerSems[i]);
    }
    
    for (pid_t pid : playerPids) {
        waitpid(pid, nullptr, 0);
    }
    
    for (sem_t* sem : playerSems) {
        sem_close(sem);
    }
    
    sem_close(mainSem);
    sem_close(moveMadeSem);
    
    for (int i = 0; i < n; i++) {
        std::string semName = PLAYER_SEM + std::to_string(i + 1);
        sem_unlink(semName.c_str());
    }
    
    sem_unlink(MAIN_SEM);
    sem_unlink(MOVE_MADE_SEM);
    
    munmap(tournamentState, sizeof(TournamentState));
    close(shm_fd);
    shm_unlink(SHM_NAME);
    
    return 0;
} 