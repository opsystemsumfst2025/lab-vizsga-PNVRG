#define _POSIX_C_SOURCE 200809L // dprintf, sigaction support
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

// --- KONFIGURÁCIÓ ---
#define NUM_TRADERS 3       // Kereskedő szálak száma
#define STARTING_CASH 10000.0
#define QUEUE_SIZE 5        // A megosztott puffer mérete

// --- ADATSTRUKTÚRÁK ---

// Láncolt lista elem a tranzakciókhoz
typedef struct Transaction {
    char details[128];
    struct Transaction* next;
} Transaction;

// Megosztott erőforrások (Piac adatok puffere)
typedef struct {
    char data[QUEUE_SIZE][64];
    int head;
    int tail;
    int count;
} MarketBuffer;

// Globális változók
double wallet_balance = STARTING_CASH;
int stocks_owned = 0;

Transaction* log_head = NULL; // Láncolt lista eleje
Transaction* log_tail = NULL; // Láncolt lista vége (optimalizálás)

MarketBuffer market_queue;    // Közös puffer
pid_t market_pid = 0;         // Gyerek folyamat ID

// Szinkronizációs eszközök
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_new_data = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_space_available = PTHREAD_COND_INITIALIZER; // Opcionális, de szép

volatile sig_atomic_t keep_running = 1; // Leállító kapcsoló

// --- SEGÉDFÜGGVÉNYEK ---

// Tranzakció hozzáadása a láncolt listához (Dynamic Memory)
void log_transaction(const char* msg) {
    Transaction* new_node = (Transaction*)malloc(sizeof(Transaction));
    if (!new_node) {
        perror("Memory allocation failed");
        return;
    }
    snprintf(new_node->details, sizeof(new_node->details), "%s", msg);
    new_node->next = NULL;

    // Mutex már zárolva van, amikor ezt hívjuk a szálakból!
    if (log_tail == NULL) {
        log_head = new_node;
        log_tail = new_node;
    } else {
        log_tail->next = new_node;
        log_tail = new_node;
    }
}

// Láncolt lista felszabadítása (Memory Leak prevention)
void free_logs() {
    Transaction* current = log_head;
    while (current != NULL) {
        Transaction* temp = current;
        current = current->next;
        free(temp);
    }
    printf("[System] Transaction logs freed.\n");
}

// --- SIGNAL HANDLER ---
void handle_sigint(int sig) {
    // Csak a flaget állítjuk, a főciklus és szálak ebből tudják, hogy vége
    keep_running = 0;
}

// --- PIAC FOLYAMAT (CHILD) ---
void market_process(int write_pipe) {
    srand(time(NULL) ^ getpid()); // Seed
    const char* tickers[] = {"AAPL", "GOOG", "TSLA", "MSFT", "AMZN"};
    int num_tickers = 5;

    while (1) { // A szülő fogja megölni (SIGTERM)
        int ticker_idx = rand() % num_tickers;
        int price = 100 + (rand() % 2900); // 100 - 3000 közötti ár
        
        // Formázott írás a pipe-ba (dprintf kényelmes)
        dprintf(write_pipe, "%s %d\n", tickers[ticker_idx], price);
        
        // Pici random késleltetés, hogy szimuláljuk a piacot
        usleep(500000 + (rand() % 1000000)); // 0.5 - 1.5 mp
    }
}

// --- KERESKEDŐ SZÁL (THREAD) ---
void* trader_thread(void* arg) {
    long id = (long)arg;
    char task[64];

    while (keep_running) {
        pthread_mutex_lock(&mutex);

        // Várakozás adatra (Condition Variable) - NEM busy wait!
        while (market_queue.count == 0 && keep_running) {
            pthread_cond_wait(&cond_new_data, &mutex);
        }

        if (!keep_running && market_queue.count == 0) {
            pthread_mutex_unlock(&mutex);
            break;
        }

        // Adat kivétele a pufferből (FIFO)
        strcpy(task, market_queue.data[market_queue.tail]);
        market_queue.tail = (market_queue.tail + 1) % QUEUE_SIZE;
        market_queue.count--;

        // Üzleti logika
        char ticker[10];
        int price;
        sscanf(task, "%s %d", ticker, &price);

        // Egyszerű stratégia: Ha van pénz, veszünk, ha van részvény, eladunk (véletlenszerűen)
        int action = rand() % 2; // 0: BUY, 1: SELL
        char log_msg[128];

        if (action == 0 && wallet_balance >= price) {
            wallet_balance -= price;
            stocks_owned++;
            snprintf(log_msg, 128, "[Trader %ld] BOUGHT %s at %d | Balance: %.2f", id, ticker, price, wallet_balance);
            log_transaction(log_msg);
            printf("%s\n", log_msg);
        } 
        else if (action == 1 && stocks_owned > 0) {
            wallet_balance += price;
            stocks_owned--;
            snprintf(log_msg, 128, "[Trader %ld] SOLD %s at %d | Balance: %.2f", id, ticker, price, wallet_balance);
            log_transaction(log_msg);
            printf("%s\n", log_msg);
        }
        else {
            // Nem tudtunk kereskedni (nincs pénz vagy nincs részvény)
            // printf("[Trader %ld] Skipped %s at %d\n", id, ticker, price);
        }

        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

// --- MAIN (PARENT) ---
int main() {
    int pipefd[2];
    
    // 0. Signal kezelés beállítása
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0; // Megszkítja a read() hívást, ami jó nekünk!
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // 1. Pipe létrehozása
    if (pipe(pipefd) == -1) {
        perror("Pipe failed");
        return 1;
    }

    // 2. Fork
    market_pid = fork();

    if (market_pid < 0) {
        perror("Fork failed");
        return 1;
    }

    if (market_pid == 0) {
        // --- GYEREK (PIAC) ---
        close(pipefd[0]); // Olvasót lezárjuk
        market_process(pipefd[1]);
        close(pipefd[1]);
        exit(0);
    }

    // --- SZÜLŐ (KERESKEDŐ MOTOR) ---
    close(pipefd[1]); // Írót lezárjuk
    
    // Puffer inicializálás
    market_queue.head = 0;
    market_queue.tail = 0;
    market_queue.count = 0;

    // Szálak indítása
    pthread_t traders[NUM_TRADERS];
    for (long i = 0; i < NUM_TRADERS; i++) {
        if (pthread_create(&traders[i], NULL, trader_thread, (void*)i) != 0) {
            perror("Failed to create thread");
        }
    }

    printf("--- Wall Street System Started (PID: %d) ---\n", getpid());
    printf("Waiting for market data... Press Ctrl+C to stop.\n");

    // Főciklus: Pipe olvasása és pufferbe töltése
    FILE* stream = fdopen(pipefd[0], "r");
    char line[64];

    while (keep_running && fgets(line, sizeof(line), stream) != NULL) {
        // Newline levágása
        line[strcspn(line, "\n")] = 0;

        pthread_mutex_lock(&mutex);
        
        // Ha tele a puffer, eldobjuk az adatot (vagy várhatnánk is)
        if (market_queue.count < QUEUE_SIZE) {
            strcpy(market_queue.data[market_queue.head], line);
            market_queue.head = (market_queue.head + 1) % QUEUE_SIZE;
            market_queue.count++;
            
            // Jelezzük az alvó szálaknak, hogy van munka
            pthread_cond_signal(&cond_new_data); 
        }
        
        pthread_mutex_unlock(&mutex);
    }

    // --- LEÁLLÍTÁS ÉS TAKARÍTÁS ---
    printf("\n--- Shutting down Wall Street ---\n");

    // 1. Piac leállítása
    kill(market_pid, SIGTERM);
    wait(NULL); // Zombie process elkerülése

    // 2. Szálak felébresztése, hogy ki tudjanak lépni
    pthread_mutex_lock(&mutex);
    keep_running = 0; // Biztos ami biztos
    pthread_cond_broadcast(&cond_new_data);
    pthread_mutex_unlock(&mutex);

    // 3. Szálak bevárása (Join)
    for (int i = 0; i < NUM_TRADERS; i++) {
        pthread_join(traders[i], NULL);
    }

    // 4. Erőforrások felszabadítása
    fclose(stream);
    close(pipefd[0]);
    free_logs();
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond_new_data);

    // 5. Végső statisztika
    printf("Final Wallet Balance: %.2f\n", wallet_balance);
    printf("Final Stocks Owned: %d\n", stocks_owned);
    printf("System exited cleanly.\n");

    return 0;
}
