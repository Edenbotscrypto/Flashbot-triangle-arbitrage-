const { ethers } = require("hardhat");
require("dotenv").config();

async function main() {
  const [deployer] = await ethers.getSigners();
  console.log("Deployer:", deployer.address);

  const routers = [
    process.env.UNISWAP_V2_ROUTER,
    process.env.SUSHISWAP_ROUTER,
    process.env.CURVE_ROUTER
  ];

  const providerAddr = process.env.AAVE_PROVIDER;
  if (!providerAddr || routers.includes(undefined)) {
    throw new Error("Missing env vars for routers or Aave provider");
  }

  const Factory = await ethers.getContractFactory("ArbitrageFlashLoan");
  const contract = await Factory.deploy(providerAddr, routers);
  await contract.deployed();

  console.log("ArbitrageFlashLoan deployed at:", contract.address);
}

main()
  .then(() => process.exit(0))
  .catch((err) => {
    console.error(err);
    process.exit(1);
  });