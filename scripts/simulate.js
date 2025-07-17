const { ethers, network } = require("hardhat");
require("dotenv").config();

async function main() {
  const loanToken = process.env.SIM_LOAN_TOKEN || "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"; // USDC
  const amount = ethers.utils.parseUnits("100000", 6); // 100k USDC

  // Deploy ArbitrageFlashLoan freshly in fork
  const providerAddr = process.env.AAVE_PROVIDER;
  const Factory = await ethers.getContractFactory("ArbitrageFlashLoan");
  const arbitrage = await Factory.deploy(providerAddr);
  await arbitrage.deployed();
  console.log("Deployed arbitrage for fork sim:", arbitrage.address);

  // Example dummy swap data (no-op) -> will revert UNPROFITABLE but illustrates call structure
  const targets = [];
  const payloads = [];
  const tokensToApprove = [];

  try {
    await arbitrage.executeArb(loanToken, amount, targets, payloads, tokensToApprove);
  } catch (err) {
    console.log("Expected revert / simulation complete:", err.message);
  }
}

main()
  .then(() => process.exit(0))
  .catch((err) => {
    console.error(err);
    process.exit(1);
  });