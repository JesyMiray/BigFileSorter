#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <sstream>
#include <atomic>
#include <memory>
#include <windows.h>
#include <commdlg.h>
#include <chrono>
#include <iomanip> 

const size_t CHUNK_SIZE = 10 * 1024 * 1024; //10 MB
std::atomic<bool> reading_done(false);

//мьютекс и условная переменная для работы с очередью
std::mutex queue_mutex;
std::condition_variable data_condition;
std::queue<std::string> data_queue;

//мьютекс для записи временных файлов
std::mutex file_mutex;
std::vector<std::string> temp_files;

/*функция вызова диалога выбора файла*/
std::wstring open_file_dialog() {
    wchar_t filename[MAX_PATH] = { 0 };

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = nullptr; // Если есть окно, передаем его дескриптор
    ofn.lpstrFilter = L"Text Files\0*.TXT\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        return std::wstring(filename);
    }
    else {
        std::wcerr << L"No file selected or error occurred." << std::endl;
        return L"";
    }
}

/*функция чтения файла*/
void file_reader(const std::string& filename) {
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "Error opening file: " << filename << std::endl;
        return;
    }

    std::unique_ptr<char[]> buffer(new char[CHUNK_SIZE]);
    std::string chunk;
    chunk.reserve(CHUNK_SIZE);

    while (infile) {
        infile.read(buffer.get(), CHUNK_SIZE);
        size_t bytes_read = infile.gcount();
        chunk.append(buffer.get(), bytes_read);

        //смотрим последний разделитель, чтобы не разрывать число
        size_t last_delim = chunk.find_last_of(" ,");
        if (last_delim != std::string::npos && bytes_read == CHUNK_SIZE) {
            std::string to_queue = chunk.substr(0, last_delim + 1);
            chunk = chunk.substr(last_delim + 1);

            //добавляем в очередь
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                data_queue.push(std::move(to_queue));
            }
            data_condition.notify_one();
        }
    }

    //последний кусок
    if (!chunk.empty()) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        data_queue.push(std::move(chunk));
    }
    reading_done = true;
    data_condition.notify_all();
}

/*рабочий поток для обработки данных*/
void worker(int thread_id) {
    while (true) {
        std::string data_chunk;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            data_condition.wait(lock, [] { return !data_queue.empty() || reading_done; });

            if (data_queue.empty() && reading_done) break;
            data_chunk = std::move(data_queue.front());
            data_queue.pop();
        }

        //парсинг чисел
        std::vector<double> numbers;
        std::istringstream stream(data_chunk);
        std::string token;
        while (std::getline(stream, token, ' ')) {
            try {
                numbers.push_back(std::stod(token));
            }
            catch (...) {}
        }

        //сортировка
        std::sort(numbers.begin(), numbers.end());

        //запись во временный файл
        std::string temp_filename = "temp_" + std::to_string(thread_id) + "_" + std::to_string(rand()) + ".txt";
        {
            std::lock_guard<std::mutex> lock(file_mutex);
            temp_files.push_back(temp_filename);
        }

        std::ofstream temp_file(temp_filename);
        for (double num : numbers) {
            temp_file << num << " ";
        }
    }
}

/*функция объединения временных файлов*/
void merge_files(const std::string& output_file) {
    std::priority_queue<std::pair<double, std::ifstream*>,
        std::vector<std::pair<double, std::ifstream*>>,
        std::greater<>> min_heap;

    std::vector<std::unique_ptr<std::ifstream>> streams;
    for (const std::string& temp_file : temp_files) {
        auto stream = std::make_unique<std::ifstream>(temp_file);
        if (!stream->is_open()) {
            std::cerr << "Error opening temp file: " << temp_file << std::endl;
            continue;
        }
        double value;
        if (*stream >> value) {
            min_heap.emplace(value, stream.get());
        }
        streams.push_back(std::move(stream));
    }

    std::ofstream outfile(output_file);
    if (!outfile.is_open()) {
        std::cerr << "Error opening output file: " << output_file << std::endl;
        return;
    }

    while (!min_heap.empty()) {
        auto [value, stream] = min_heap.top();
        min_heap.pop();
        outfile << value << " ";
        if (*stream >> value) {
            min_heap.emplace(value, stream);
        }
    }

    //закрытие временных файлов
    for (const auto& stream : streams) {
        if (stream && stream->is_open()) {
            stream->close();
        }
    }
}

int main() {
    //засекаем начало работы
    auto start_time = std::chrono::high_resolution_clock::now();

    //вызов диалога для выбора файла
    std::wstring input_file_w = open_file_dialog();
    if (input_file_w.empty()) {
        std::wcerr << L"No file selected. Exiting." << std::endl;
        return 1;
    }

    //преобразование в std::string для дальнейшей работы
    std::string input_file(input_file_w.begin(), input_file_w.end());
    const std::string output_file = "output.txt";

    //поток для чтения
    std::thread reader_thread(file_reader, input_file);

    //потоки для обработки
    const int num_threads = std::thread::hardware_concurrency();
    std::vector<std::thread> worker_threads;
    for (int i = 0; i < num_threads; ++i) {
        worker_threads.emplace_back(worker, i);
    }

    //ожидание завершения потоков
    reader_thread.join();
    for (auto& thread : worker_threads) {
        thread.join();
    }

    //объединяем временные файлы
    merge_files(output_file);

    //засекаем конец работы
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    std::cout << "Sorting complete. Output written to " << output_file << std::endl;
    std::cout << "Execution time: " << duration.count() << " seconds" << std::endl;

    return 0;
}
