const { ethers } = require("hardhat");
require("dotenv").config();

async function main() {
  const [deployer] = await ethers.getSigners();
  console.log("Deployer:", deployer.address);

  const providerAddr = process.env.AAVE_PROVIDER;
  if (!providerAddr) {
    throw new Error("Missing env var AAVE_PROVIDER");
  }

  const Factory = await ethers.getContractFactory("ArbitrageFlashLoan");
  const contract = await Factory.deploy(providerAddr);
  await contract.deployed();

  console.log("ArbitrageFlashLoan deployed at:", contract.address);
}

main()
  .then(() => process.exit(0))
  .catch((err) => {
    console.error(err);
    process.exit(1);
  });