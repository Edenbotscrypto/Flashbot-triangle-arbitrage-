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
    address[3] public routers;               // DEX routers (e.g. Uni, Sushi, Curve)

    modifier onlyOwner() { require(msg.sender == owner, "NOT_OWNER"); _; }

    event ArbitrageExecuted(uint256 profit, uint256 gasPrice);

    constructor(
        ILendingPoolAddressesProvider _provider,
        address[3] memory _routers
    ) FlashLoanReceiverBase(_provider) public {
        owner   = msg.sender;
        routers = _routers;
    }

    /**
     * @dev Called off-chain to kick-off a trade. All paths must form a cycle.
     */
    function executeArb(
        address[] calldata path1,
        address[] calldata path2,
        address[] calldata path3,
        uint256        amountIn
    ) external onlyOwner {
        require(path1[0] == path3[path3.length - 1], "CYCLE_MISMATCH");

        address[] memory assets  = new address[](1);
        uint256[] memory amounts = new uint256[](1);
        uint256[] memory modes   = new uint256[](1);

        assets[0]  = path1[0];
        amounts[0] = amountIn;
        modes[0]   = 0; // no debt — full repayment at end

        bytes memory params = abi.encode(path1, path2, path3);

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
        (address[] memory p1, address[] memory p2, address[] memory p3) = abi.decode(params, (address[], address[], address[]));

        // 1️⃣ First swap
        IERC20(assets[0]).safeApprove(routers[0], amounts[0]);
        uint[] memory r1 = IUniswapV2Router02(routers[0]).swapExactTokensForTokens(amounts[0], 0, p1, address(this), block.timestamp);
        uint amountAfter1 = r1[r1.length - 1];

        // 2️⃣ Second swap
        IERC20(p2[0]).safeApprove(routers[1], amountAfter1);
        uint[] memory r2 = IUniswapV2Router02(routers[1]).swapExactTokensForTokens(amountAfter1, 0, p2, address(this), block.timestamp);
        uint amountAfter2 = r2[r2.length - 1];

        // 3️⃣ Third swap
        IERC20(p3[0]).safeApprove(routers[2], amountAfter2);
        uint[] memory r3 = IUniswapV2Router02(routers[2]).swapExactTokensForTokens(amountAfter2, 0, p3, address(this), block.timestamp);
        uint finalAmount = r3[r3.length - 1];

        uint amountOwing = amounts[0].add(premiums[0]);
        require(finalAmount > amountOwing, "UNPROFITABLE");

        // Repay Aave & pocket spread
        IERC20(assets[0]).safeApprove(address(LENDING_POOL), amountOwing);
        uint profit = finalAmount.sub(amountOwing);
        IERC20(assets[0]).safeTransfer(owner, profit);

        emit ArbitrageExecuted(profit, tx.gasprice);
        return true;
    }

    /* ========== ADMIN ========== */
    function updateRouters(address[3] calldata _routers) external onlyOwner { routers = _routers; }
    function sweep(address token) external onlyOwner {
        IERC20 t = IERC20(token);
        t.safeTransfer(owner, t.balanceOf(address(this)));
    }
}