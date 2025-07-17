// SPDX-License-Identifier: MIT
pragma solidity ^0.6.12;
pragma experimental ABIEncoderV2;

import "@openzeppelin/contracts/token/ERC20/IERC20.sol";
import "@openzeppelin/contracts/token/ERC20/SafeERC20.sol";
import "@openzeppelin/contracts/math/SafeMath.sol";
import "@openzeppelin/contracts/utils/ReentrancyGuard.sol";

import "@aave/protocol-v2/contracts/flashloan/base/FlashLoanReceiverBase.sol";
import "@aave/protocol-v2/contracts/interfaces/ILendingPoolAddressesProvider.sol";
import "@aave/protocol-v2/contracts/interfaces/ILendingPool.sol";

// Minimal subset of Uniswap-style router. Extend as needed.
interface IUniswapV2Router02 {
    function swapExactTokensForTokens(
        uint amountIn,
        uint amountOutMin,
        address[] calldata path,
        address to,
        uint deadline
    ) external returns (uint[] memory amounts);
}

/**
 * @title ArbitrageFlashLoan
 * @notice Receives an Aave v2 flash-loan, performs 3 hops across distinct DEXs,
 *         then repays loan and keeps the delta as profit. Unprofitable tx → revert.
 */
contract ArbitrageFlashLoan is FlashLoanReceiverBase, ReentrancyGuard {
    using SafeERC20 for IERC20;
    using SafeMath  for uint256;

    address public immutable owner;          // bot controller wallet
    // Routers no longer stored on-chain; provided dynamically per tx for max flexibility

    modifier onlyOwner() { require(msg.sender == owner, "NOT_OWNER"); _; }

    event ArbitrageExecuted(uint256 profit, uint256 gasPrice);

    constructor(ILendingPoolAddressesProvider _provider) FlashLoanReceiverBase(_provider) public {
        owner = msg.sender;
    }

    /**
     * @notice Kick-off an arbitrary sequence of swaps. The bot provides:
     *         – loanToken: asset to borrow via flash-loan and the final asset repaid
     *         – amountIn : quantity to borrow
     *         – targets  : contract addresses to call sequentially (routers/pools)
     *         – payloads : calldata for each target (must include `to = address(this)` where necessary)
     *         – tokensToApprove: input token for each call that the contract should approve to target[i]
     *
     *  Requirements:
     *  1. targets.length == payloads.length == tokensToApprove.length
     *  2. The final call must end with loanToken balance >= amountOwing (checked in executeOperation)
     */
    function executeArb(
        address   loanToken,
        uint256   amountIn,
        address[] calldata targets,
        bytes[]   calldata payloads,
        address[] calldata tokensToApprove,
        uint256   minProfitWei
    ) external onlyOwner {
        require(
            targets.length == payloads.length && targets.length == tokensToApprove.length,
            "LEN_MISMATCH"
        );

        address[] memory assets  = new address[](1);
        uint256[] memory amounts = new uint256[](1);
        uint256[] memory modes   = new uint256[](1);

        assets[0]  = loanToken;
        amounts[0] = amountIn;
        modes[0]   = 0; // no debt – full repayment in same tx

        bytes memory params = abi.encode(targets, payloads, tokensToApprove, minProfitWei);

        LENDING_POOL.flashLoan(
            address(this),
            assets,
            amounts,
            modes,
            address(this),
            params,
            0
        );
    }

    /**
     * @inheritdoc FlashLoanReceiverBase
     */
    function executeOperation(
        address[] calldata assets,
        uint256[] calldata amounts,
        uint256[] calldata premiums,
        address /* initiator */,
        bytes   calldata params
    ) external override nonReentrant returns (bool) {
        (address[] memory targets, bytes[] memory payloads, address[] memory tokensToApprove, uint256 minProfitWei) = abi.decode(params, (address[], bytes[], address[], uint256));

        require(targets.length == payloads.length && targets.length == tokensToApprove.length, "BAD_DECODE");

        // Execute each swap or arbitrary call in sequence
        for (uint256 i = 0; i < targets.length; i++) {
            // Approve if required (gas-optimised: approval only if allowance == 0)
            address token = tokensToApprove[i];
            if (token != address(0)) {
                uint256 allowance = IERC20(token).allowance(address(this), targets[i]);
                if (allowance == 0) {
                    IERC20(token).safeApprove(targets[i], uint(-1));
                }
            }

            (bool success, bytes memory ret) = targets[i].call(payloads[i]);
            require(success, string(abi.encodePacked("CALL_FAIL_", i)));
            ret; // silence solidity-unused
        }

        uint finalAmount = IERC20(assets[0]).balanceOf(address(this));

        uint amountOwing = amounts[0].add(premiums[0]);
        require(finalAmount >= amountOwing.add(minProfitWei), "UNPROFITABLE");

        // Repay Aave & pocket spread
        IERC20(assets[0]).safeApprove(address(LENDING_POOL), amountOwing);
        uint profit = finalAmount.sub(amountOwing);
        IERC20(assets[0]).safeTransfer(owner, profit);

        emit ArbitrageExecuted(profit, tx.gasprice);
        return true;
    }

    /* ========== ADMIN ========== */
    // Deprecated; kept for backward compatibility – does nothing
    function updateRouters(address[3] calldata /*_routers*/) external onlyOwner {}
    function sweep(address token) external onlyOwner {
        IERC20 t = IERC20(token);
        t.safeTransfer(owner, t.balanceOf(address(this)));
    }
}