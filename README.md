# Flash-Loan Triangular Arbitrage Bot

> 🚀 Proof-of-concept repo for an Aave-powered, high-frequency triangular arbitrage executor on Ethereum main-net.

---

## Folder Layout

| Path | Description |
|------|-------------|
| `contracts/ArbitrageFlashLoan.sol` | Solidity executor that receives the flash loan and performs the 3-hop swap cycle. |
| `scanner/` | C++ scanner that hunts for mis-pricings in real-time. |
| `scripts/` | Hardhat scripts for deploy & management. |
| `hardhat.config.js` | Hardhat config with main-net forking. |

## Quick Start

1. **Install deps**
   ```bash
   yarn install         # JS deps
   sudo apt-get install libcurl4-openssl-dev       # for cpr (Ubuntu)
   ```
2. **Configure**
   ```bash
   cp .env.example .env && $EDITOR .env   # fill RPC_URL, PRIVATE_KEY, etc.
   ```
3. **Compile & Deploy**
   ```bash
   npx hardhat compile
   npx hardhat run scripts/deploy.js --network mainnet  # or hardhat (fork)
   ```
4. **Build Scanner**
   ```bash
   cd scanner && g++ -std=c++17 arbitrageScanner.cpp -lcpr -lpthread -o scanner && ./scanner
   ```

> The scanner prints out candidate paths; extend it to auto-send bundles to Flashbots ⛽.

## Deployment to Polygon / Arbitrum

1. Update `.env` with `POLYGON_RPC_URL` / `ARBITRUM_RPC_URL` and `PRIVATE_KEY`.
2. Compile & deploy:
   ```bash
   npx hardhat run scripts/deploy.js --network polygon   # or arbitrum
   ```

> The same Solidity bytecode works on L2; just pass the Aave v3 addresses for the respective network.

## Telegram Alerts
Set `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` in `.env` and the scanner will push a message each time a profitable triangle is detected & bundle submitted.

## To Do
- Finish on-chain price/reserve pulling in C++
- Add gas estimator & ML profit scorer
- Integrate Flashbots bundle submission

---
MIT © 2025