# GoQuant-Deribit

A comprehensive trading system application that interacts with the Deribit API to perform various trading operations. This application includes functionalities for WebSocket communication, HTTP requests, and command-line interaction.

## Features

- WebSocket server for client subscriptions
- Real-time order book updates
- Place, cancel, and modify orders
- Retrieve order book and position details
- List open orders
- Command-line interface for user interaction

## Prerequisites

Ensure you have the following libraries installed:

- Boost
- OpenSSL
- cURL
- pthread

On macOS, you can use Homebrew to install these dependencies:

```bash
brew install boost openssl curl
```


## Installation

1. Clone the repository:

   ```bash
   git clone https://github.com/VishnuSrivatsava/GoQuant-Deribit.git
   cd GoQuant-Deribit
   ```

2. Compile the code using `g++`:

   ```bash
   g++ -std=c++11 -I/opt/homebrew/opt/boost/include -I/opt/homebrew/opt/openssl/include -o trading_system main.cpp -L/opt/homebrew/opt/boost/lib -L/opt/homebrew/opt/openssl/lib -lboost_system-mt -lboost_thread-mt -lssl -lcrypto -lcurl -lpthread -arch arm64
   ```

## Usage

1. Run the compiled binary:

   ```bash
   ./trading_system
   ```

2. Follow the command-line prompts to interact with the trading system. Available commands include:

   - `subscribe`: Subscribe to a market data stream.
   - `place`: Place a limit order.
   - `cancel`: Cancel an existing order.
   - `modify`: Modify an existing order.
   - `orderbook`: Retrieve the order book for a specific instrument.
   - `position`: Get position details for a specific instrument.
   - `openorders`: List all open orders.
   - `exit`: Exit the application.

## Configuration

Replace the placeholder client credentials in `main.cpp` with your actual Deribit API client ID and secret:

```cpp
  std::string clientId = "your_client_id";
  std::string clientSecret = "your_client_secret";
```


## Logging

- Market data is logged to `market_data.log`.
- Latency data is logged to `latency_data.log`


