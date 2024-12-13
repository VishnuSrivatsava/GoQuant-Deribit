#include <iostream>
#include <string>
#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <fstream>
#include <queue>
#include <condition_variable>
#include <chrono>
#include "json.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio.hpp>

std::condition_variable messageCondition;

using json = nlohmann::json;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Global log file
std::ofstream logFile("market_data.log", std::ios::app);
std::ofstream latencyLogFile("latency_data.log", std::ios::app); // Log file for latency data
std::mutex outputMutex;
std::queue<std::string> messageQueue;
bool running = true;

// Subscriptions map
std::unordered_map<std::string, std::set<websocket::stream<tcp::socket>*>> subscriptions;

// Callback function to handle the response from the cURL request
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Function to log data
void logData(const std::string& data) {
    if (logFile.is_open()) {
        logFile << data << std::endl;
    } else {
        std::cerr << "Unable to open log file." << std::endl;
    }
}

// Function to log latency data
void logLatencyData(const std::string& data) {
    if (latencyLogFile.is_open()) {
        latencyLogFile << data << std::endl;
    } else {
        std::cerr << "Unable to open latency log file." << std::endl;
    }
}

// Function to send a cURL request with optional access token
std::string sendRequest(const std::string& url, const json& payload, const std::string& accessToken = "") {
    std::string responseBuffer;
    CURL* curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        // Convert JSON payload to string and set it as POST data
        std::string jsonStr = payload.dump();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());

        // Set headers, including Authorization if accessToken is provided
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!accessToken.empty()) {
            headers = curl_slist_append(headers, ("Authorization: Bearer " + accessToken).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Set up the write callback to capture the response
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);

        // Perform the request
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "Request failed: " << curl_easy_strerror(res) << std::endl;
        }

        // Clean up
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return responseBuffer;
}

// Function to obtain an access token
std::string getAccessToken(const std::string& clientId, const std::string& clientSecret) {
    json payload = {
        {"id", 0},
        {"method", "public/auth"},
        {"params", {
            {"grant_type", "client_credentials"},
            {"scope", "session:apiconsole-c5i26ds6dsr expires:2592000"},
            {"client_id", clientId},
            {"client_secret", clientSecret}
        }},
        {"jsonrpc", "2.0"}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/public/auth", payload);
    auto responseJson = json::parse(response);

    // Extract the access token from the response
    if (responseJson.contains("result") && responseJson["result"].contains("access_token")) {
        return responseJson["result"]["access_token"];
    } else {
        std::cerr << "Failed to retrieve access token." << std::endl;
        return "";
    }
}

// Thread pool class
class ThreadPool {
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();

    void enqueue(std::function<void()> task);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

ThreadPool::ThreadPool(size_t numThreads) : stop(false) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queueMutex);
                    this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                    if (this->stop && this->tasks.empty()) return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread &worker : workers) {
        worker.join();
    }
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        tasks.emplace(std::move(task));
    }
    condition.notify_one();
}

// WebSocket server to handle client subscriptions and stream orderbook updates
void runWebSocketServer() {
    try {
        net::io_context ioc;
        tcp::acceptor acceptor{ioc, tcp::endpoint(tcp::v4(), 9002)};

        while (running) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);

            // Modify the lambda to avoid initialized captures
            std::thread([&socket]() mutable {
                websocket::stream<tcp::socket> ws{std::move(socket)};
                ws.accept();

                while (running) {
                    beast::flat_buffer buffer;
                    ws.read(buffer);
                    std::string message = beast::buffers_to_string(buffer.data());

                    json request = json::parse(message);
                    std::string action = request["action"];
                    std::string symbol = request["symbol"];

                    if (action == "subscribe") {
                        // Access the global subscriptions map directly
                        subscriptions[symbol].insert(&ws);
                        ws.write(net::buffer("{\"status\":\"subscribed\",\"symbol\":\"" + symbol + "\"}"));
                    } else if (action == "unsubscribe") {
                        // Access the global subscriptions map directly
                        subscriptions[symbol].erase(&ws);
                        ws.write(net::buffer("{\"status\":\"unsubscribed\",\"symbol\":\"" + symbol + "\"}"));
                    }
                }
            }).detach();
        }
    } catch (std::exception const& e) {
        std::cerr << "WebSocket Server Error: " << e.what() << std::endl;
    }
}

// Function to simulate and stream orderbook updates
void streamOrderbookUpdates() {
    while (running) {
        for (auto it = subscriptions.begin(); it != subscriptions.end(); ++it) {
            const std::string& symbol = it->first;
            const std::set<websocket::stream<tcp::socket>*>& clients = it->second;

            json orderbook;
            orderbook["symbol"] = symbol;
            orderbook["bids"] = json::array(); // Simulate bids
            orderbook["asks"] = json::array(); // Simulate asks

            std::string update = orderbook.dump();

            for (auto client = clients.begin(); client != clients.end(); ++client) {
                (*client)->write(net::buffer(update));
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1)); // Simulate update interval
    }
}

// Function to place a limit order
void placeLimitOrder(const std::string& price, const std::string& accessToken, const std::string& amount, const std::string& instrument) {
    auto start = std::chrono::high_resolution_clock::now();

    json payload = {
        {"jsonrpc", "2.0"},
        {"method", "private/buy"},
        {"params", {
            {"instrument_name", instrument},
            {"type", "limit"},
            {"price", price},
            {"amount", amount}
        }},
        {"id", 1}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/private/buy", payload, accessToken);
    auto responseJson = json::parse(response);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    logLatencyData("Order placement latency: " + std::to_string(duration.count()) + " seconds");

    // Print the entire response for debugging
    std::cout << "Full Response: " << responseJson.dump(4) << std::endl;

    // Check for errors in the response
    if (responseJson.contains("error")) {
        std::cout << "Order Placement Failed:\n";
        std::cout << "Error Code: " << responseJson["error"]["code"] << "\n";
        std::cout << "Error Message: " << responseJson["error"]["message"] << "\n";
    } else if (responseJson.contains("result") && responseJson["result"].contains("order")) {
        std::cout << "Order Placed Successfully:\n";
        auto order = responseJson["result"]["order"];
        std::cout << "Order ID: " << (order.contains("order_id") ? order["order_id"].get<std::string>() : "N/A") << "\n";
        std::cout << "Instrument: " << (order.contains("instrument_name") ? order["instrument_name"].get<std::string>() : "N/A") << "\n";
        std::cout << "Price: " << (order.contains("price") ? std::to_string(order["price"].get<double>()) : "N/A") << "\n";
        std::cout << "Amount: " << (order.contains("amount") ? std::to_string(order["amount"].get<double>()) : "N/A") << "\n";
    } else {
        std::cout << "Unexpected response format.\n";
    }
}

// Function to cancel an existing order
void cancelOrder(const std::string& accessToken, const std::string& orderID) {
    auto start = std::chrono::high_resolution_clock::now();

    json payload = {
        {"jsonrpc", "2.0"},
        {"method", "private/cancel"},
        {"params", {{"order_id", orderID}}},
        {"id", 6}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/private/cancel", payload, accessToken);
    auto responseJson = json::parse(response);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    logLatencyData("Order cancellation latency: " + std::to_string(duration.count()) + " seconds");

    if (responseJson.contains("error")) {
        std::cout << "Order Cancellation Failed:\n";
        std::cout << "Error Code: " << responseJson["error"]["code"] << "\n";
        std::cout << "Error Message: " << responseJson["error"]["message"] << "\n";
    } else {
        std::cout << "Order Cancelled Successfully:\n";
        std::cout << "Order ID: " << responseJson["result"]["order_id"] << "\n";
    }
}

// Function to modify an existing order
void modifyOrder(const std::string& accessToken, const std::string& orderID, int amount, double price) {
    auto start = std::chrono::high_resolution_clock::now();

    json payload = {
        {"jsonrpc", "2.0"},
        {"method", "private/edit"},
        {"params", {
            {"order_id", orderID},
            {"amount", amount},
            {"price", price}
        }},
        {"id", 11}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/private/edit", payload, accessToken);
    auto responseJson = json::parse(response);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    logLatencyData("Order modification latency: " + std::to_string(duration.count()) + " seconds");

    // Print the entire response for debugging
    std::cout << "Full Response: " << responseJson.dump(4) << std::endl;

    if (responseJson.contains("error")) {
        std::cout << "Order Modification Failed:\n";
        std::cout << "Error Code: " << responseJson["error"]["code"] << "\n";
        std::cout << "Error Message: " << responseJson["error"]["message"] << "\n";
    } else if (responseJson.contains("result") && responseJson["result"].contains("order")) {
        std::cout << "Order Modified Successfully:\n";
        auto order = responseJson["result"]["order"];
        std::cout << "Order ID: " << (order.contains("order_id") ? order["order_id"].get<std::string>() : "N/A") << "\n";
        std::cout << "New Price: " << (order.contains("price") ? std::to_string(order["price"].get<double>()) : "N/A") << "\n";
        std::cout << "New Amount: " << (order.contains("amount") ? std::to_string(order["amount"].get<double>()) : "N/A") << "\n";
    } else {
        std::cout << "Unexpected response format.\n";
    }
}

// Function to retrieve the order book for a specific instrument
void getOrderBook(const std::string& accessToken, const std::string& instrument) {
    auto start = std::chrono::high_resolution_clock::now();

    json payload = {
        {"jsonrpc", "2.0"},
        {"method", "public/get_order_book"},
        {"params", {{"instrument_name", instrument}}},
        {"id", 15}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/public/get_order_book", payload, accessToken);
    auto responseJson = json::parse(response);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    logLatencyData("Order book retrieval latency: " + std::to_string(duration.count()) + " seconds");

    std::cout << "Order Book for " << instrument << ":\n\n";

    // Display best bid and ask
    std::cout << "Best Bid Price: " << responseJson["result"]["best_bid_price"] 
              << ", Amount: " << responseJson["result"]["best_bid_amount"] << '\n';
    std::cout << "Best Ask Price: " << responseJson["result"]["best_ask_price"] 
              << ", Amount: " << responseJson["result"]["best_ask_amount"] << '\n';

    // Display detailed asks
    std::cout << "\nAsks:\n";
    for (const auto& ask : responseJson["result"]["asks"]) {
        std::cout << "Price: " << ask[0] << ", Amount: " << ask[1] << '\n';
    }

    // Display detailed bids
    std::cout << "\nBids:\n";
    for (const auto& bid : responseJson["result"]["bids"]) {
        std::cout << "Price: " << bid[0] << ", Amount: " << bid[1] << '\n';
    }

    // Additional information
    std::cout << "\nMark Price: " << responseJson["result"]["mark_price"] << '\n';
    std::cout << "Open Interest: " << responseJson["result"]["open_interest"] << '\n';
    std::cout << "Timestamp: " << responseJson["result"]["timestamp"] << '\n';
}

// Function to get position details for a specific instrument
void getPosition(const std::string& accessToken, const std::string& instrument) {
    auto start = std::chrono::high_resolution_clock::now();

    json payload = {
        {"jsonrpc", "2.0"},
        {"method", "private/get_position"},
        {"params", {{"instrument_name", instrument}}},
        {"id", 20}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/private/get_position", payload, accessToken);
    auto responseJson = json::parse(response);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    logLatencyData("Position retrieval latency: " + std::to_string(duration.count()) + " seconds");

    std::cout << "Full Response: " << responseJson.dump(4) << std::endl;
    // Display position details if available
    if (responseJson.contains("result")) {
        std::cout << "Position Details for " << instrument << ":\n\n";
        auto result = responseJson["result"];
        std::cout << "Estimated Liquidation Price: " << result["estimated_liquidation_price"] << '\n';
        std::cout << "Size Currency: " << result["size_currency"] << '\n';
        std::cout << "Realized Funding: " << result["realized_funding"] << '\n';
        std::cout << "Total Profit Loss: " << result["total_profit_loss"] << '\n';
        std::cout << "Realized Profit Loss: " << result["realized_profit_loss"] << '\n';
        std::cout << "Floating Profit Loss: " << result["floating_profit_loss"] << '\n';
        std::cout << "Leverage: " << result["leverage"] << '\n';
        std::cout << "Average Price: " << result["average_price"] << '\n';
        std::cout << "Delta: " << result["delta"] << '\n';
        std::cout << "Interest Value: " << result["interest_value"] << '\n';
        std::cout << "Mark Price: " << result["mark_price"] << '\n';
        std::cout << "Settlement Price: " << result["settlement_price"] << '\n';
        std::cout << "Index Price: " << result["index_price"] << '\n';
        std::cout << "Direction: " << result["direction"] << '\n';
        std::cout << "Open Orders Margin: " << result["open_orders_margin"] << '\n';
        std::cout << "Initial Margin: " << result["initial_margin"] << '\n';
        std::cout << "Maintenance Margin: " << result["maintenance_margin"] << '\n';
        std::cout << "Kind: " << result["kind"] << '\n';
        std::cout << "Size: " << result["size"] << '\n';
    } else {
        std::cerr << "Error: Could not retrieve position data." << std::endl;
    }
}

// Function to list all open orders
void listOpenOrders(const std::string& accessToken) {
    auto start = std::chrono::high_resolution_clock::now();

    json payload = {
        {"jsonrpc", "2.0"},
        {"method", "private/get_open_orders"},
        {"params", {{"kind", "future"}, {"type", "limit"}}},
        {"id", 25}
    };

    std::string response = sendRequest("https://test.deribit.com/api/v2/private/get_open_orders", payload, accessToken);
    auto responseJson = json::parse(response);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    logLatencyData("Open orders retrieval latency: " + std::to_string(duration.count()) + " seconds");

    // Check if the response contains the "result" array
    if (responseJson.contains("result") && !responseJson["result"].empty()) {
        std::cout << "Open Orders:\n";
        std::cout << "----------------------------------------\n";
        for (const auto& order : responseJson["result"]) {
            std::string instrument = order["instrument_name"];
            std::string orderId = order["order_id"];
            double price = order["price"];
            double amount = order["amount"];
            std::string orderState = order["order_state"];

            std::cout << "Instrument: " << instrument << "\n"
                      << "Order ID: " << orderId << "\n"
                      << "Price: " << price << "\n"
                      << "Amount: " << amount << "\n"
                      << "Order State: " << orderState << "\n"
                      << "----------------------------------------\n";
        }
    } else {
        std::cout << "No open orders found.\n";
    }
}

// WebSocket client to handle real-time market data streaming
void runWebSocketClient(const std::string& clientId, const std::string& clientSecret, const std::string& instrument) {
    try {
        std::string host = "test.deribit.com";
        std::string port = "443";

        net::io_context ioc;
        ssl::context ctx{ssl::context::sslv23_client};

        // Load the root certificates into the context
        ctx.set_default_verify_paths();

        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

        auto const results = resolver.resolve(host, port);
        net::connect(ws.next_layer().next_layer(), results.begin(), results.end());
        ws.next_layer().handshake(ssl::stream_base::client);
        ws.handshake(host, "/ws/api/v2");

        // Authenticate
        std::string authMessage = R"({"jsonrpc":"2.0","method":"public/auth","params":{"grant_type":"client_credentials","client_id":")" + clientId + R"(","client_secret":")" + clientSecret + R"("},"id":1})";
        ws.write(net::buffer(authMessage));

        beast::flat_buffer buffer;
        ws.read(buffer);
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            //std::cout << "Auth Response: " << beast::make_printable(buffer.data()) << std::endl;
        }
        buffer.consume(buffer.size());

        // Subscribe to the specified instrument
        std::string subscribeMessage = R"({"jsonrpc":"2.0","method":"public/subscribe","params":{"channels":["book.)" + instrument + R"(.raw"]},"id":42})";
        ws.write(net::buffer(subscribeMessage));

        // Read messages
        while (running) {
            auto start = std::chrono::high_resolution_clock::now();

            ws.read(buffer);
            std::string message = beast::buffers_to_string(buffer.data());
            logData(message);  // Log the data

            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> duration = end - start;
            logLatencyData("WebSocket message processing latency: " + std::to_string(duration.count()) + " seconds");

            // Add message to queue
            {
                std::lock_guard<std::mutex> lock(outputMutex);
                messageQueue.push(message);
                messageCondition.notify_one();  // Notify the condition variable
            }
            buffer.consume(buffer.size());

            // Sleep to prevent flooding the console
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        ws.close(websocket::close_code::normal);
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

// Function to display messages from the queue
void displayMessages() {
    while (running) {
        std::unique_lock<std::mutex> lock(outputMutex);
        messageCondition.wait_for(lock, std::chrono::milliseconds(500), [] { return !messageQueue.empty(); });

        while (!messageQueue.empty()) {
            // Log the market data instead of printing it to the console
            logData(messageQueue.front());
            messageQueue.pop();
        }
    }
}

// Command-line interface for user interaction
void runCLI(const std::string& accessToken, const std::string& clientId, const std::string& clientSecret) {
    std::string command;
    while (true) {
        std::cout << "Enter command (subscribe, place, cancel, modify, orderbook, position, openorders, exit): ";
        std::cout.flush();  // Ensure the prompt is displayed immediately
        std::cin >> command;
        if (command == "subscribe") {
            std::string instrument;
            std::cout << "Enter instrument (e.g., ETH-PERPETUAL): ";
            std::cin >> instrument;
            std::thread clientThread(runWebSocketClient, clientId, clientSecret, instrument);
            clientThread.detach();
        } else if (command == "place") {
            std::string price, amount, instrument;
            std::cout << "Enter price: ";
            std::cin >> price;
            std::cout << "Enter amount: ";
            std::cin >> amount;
            std::cout << "Enter instrument: ";
            std::cin >> instrument;
            placeLimitOrder(price, accessToken, amount, instrument);
        } else if (command == "cancel") {
            std::string orderId;
            std::cout << "Enter order ID: ";
            std::cin >> orderId;
            cancelOrder(accessToken, orderId);
        } else if (command == "modify") {
            std::string orderId;
            int amount;
            double price;
            std::cout << "Enter order ID: ";
            std::cin >> orderId;
            std::cout << "Enter new amount: ";
            std::cin >> amount;
            std::cout << "Enter new price: ";
            std::cin >> price;
            modifyOrder(accessToken, orderId, amount, price);
        } else if (command == "orderbook") {
            std::string instrument;
            std::cout << "Enter instrument: ";
            std::cin >> instrument;
            getOrderBook(accessToken, instrument);
        } else if (command == "position") {
            std::string instrument;
            std::cout << "Enter instrument: ";
            std::cin >> instrument;
            getPosition(accessToken, instrument);
        } else if (command == "openorders") {
            listOpenOrders(accessToken);
        } else if (command == "exit") {
            running = false;
            break;
        } else {
            std::cout << "Unknown command." << std::endl;
        }
    }
}

int main() {
    // Replace with your actual client credentials
    std::string clientId = "";
    std::string clientSecret = "";

    // Retrieve the access token
    std::string accessToken = getAccessToken(clientId, clientSecret);

    if (!accessToken.empty()) {
        ThreadPool pool(4); // Create a thread pool with 4 threads

        // Enqueue WebSocket server task
        pool.enqueue([] {
            runWebSocketServer();
        });

        // Enqueue orderbook updates task
        pool.enqueue([] {
            streamOrderbookUpdates();
        });

        // Enqueue message display task
        pool.enqueue([] {
            displayMessages();
        });

        // Run the command-line interface
        runCLI(accessToken, clientId, clientSecret);
    } else {
        std::cerr << "Unable to obtain access token, aborting operations." << std::endl;
    }

    return 0;
}