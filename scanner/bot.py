import os, json, time, math
from itertools import permutations
from typing import List, Tuple, Dict

from web3 import Web3
from eth_abi import encode_abi
from eth_account import Account
from flashbots import flashbot, w3 as fb_w3


RPC      = os.environ.get("RPC_URL")
PRIVATE  = os.environ.get("PRIVATE_KEY")
FLASHBOTS_RELAY = os.environ.get("FLASHBOTS_RELAY", "https://mev-relay.flashbots.net")
ARBITRAGE_ADDR  = Web3.toChecksumAddress(os.environ["ARBITRAGE_ADDR"])
AAVE_PROVIDER   = Web3.toChecksumAddress(os.environ["AAVE_PROVIDER"])

w3 = Web3(Web3.HTTPProvider(RPC))
mev_signer = Account.from_key(PRIVATE)
flashbot(w3, mev_signer, FLASHBOTS_RELAY)

# ---------- Tokens -------------
TOKENS: Dict[str, Dict] = {
    "WETH":  {"addr": Web3.toChecksumAddress("0xC02aaA39b223FE8D0a0e5C4F27eAD9083C756Cc2"), "decimals": 18},
    "USDC":  {"addr": Web3.toChecksumAddress("0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"), "decimals": 6},
    "DAI":   {"addr": Web3.toChecksumAddress("0x6B175474E89094C44Da98b954EedeAC495271d0F"), "decimals": 18},
    "WBTC":  {"addr": Web3.toChecksumAddress("0x2260FAC5E5542a773Aa44fBCfeDf7C193bc2C599"), "decimals": 8},
}

ROUTER_V2 = Web3.toChecksumAddress(os.environ.get("UNISWAP_V2_ROUTER", "0x7a250d5630B4cF539739dF2C5dAcb4c659F2488D"))
SUSHI_ROUTER = Web3.toChecksumAddress(os.environ.get("SUSHISWAP_ROUTER", "0xd9e1cE17f2641f24aE83637ab66a2cca9C378B9F"))
CURVE_POOL = Web3.toChecksumAddress(os.environ.get("CURVE_POOL", "0xDC24316b9AE028F1497c275EB9192a3Ea0f67022"))

PAIR_ABI = json.loads('[{"constant":true,"inputs":[],"name":"getReserves","outputs":[{"internalType":"uint112","name":"reserve0","type":"uint112"},{"internalType":"uint112","name":"reserve1","type":"uint112"},{"internalType":"uint32","name":"blockTimestampLast","type":"uint32"}],"payable":false,"stateMutability":"view","type":"function"}]')
ROUTER_ABI = json.loads('[{"name":"swapExactTokensForTokens","type":"function","inputs":[{"name":"amountIn","type":"uint256"},{"name":"amountOutMin","type":"uint256"},{"name":"path","type":"address[]"},{"name":"to","type":"address"},{"name":"deadline","type":"uint256"}],"outputs":[{"type":"uint256[]","name":"amounts"}],"stateMutability":"nonpayable"}]')
router_contract = w3.eth.contract(address=ROUTER_V2, abi=ROUTER_ABI)

FACTORY_V2 = Web3.toChecksumAddress("0x5C69bEe701ef814a2B6a3EDD4B1652CB9cc5aA6f")
INIT_CODE_HASH = Web3.keccak(text="").hex()  # placeholder – use real init code in prod


# ----------------- Helpers ---------------------

def pair_address(tokenA: str, tokenB: str) -> str:
    # For demo purposes call getPair via eth_call
    data = "0xe6a43905" + tokenA[2:].rjust(64, "0") + tokenB[2:].rjust(64, "0")
    try:
        res = w3.eth.call({"to": FACTORY_V2, "data": data})
        addr = Web3.toChecksumAddress("0x" + res.hex()[26:])
        return addr
    except Exception:
        return "0x0000000000000000000000000000000000000000"


def get_reserves(pair: str):
    if pair == "0x0000000000000000000000000000000000000000":
        return None
    c = w3.eth.contract(address=pair, abi=PAIR_ABI)
    try:
        r = c.functions.getReserves().call()
        return r[:2]
    except Exception:
        return None


def amount_out_v2(amount_in: int, reserve_in: int, reserve_out: int):
    amount_in_with_fee = amount_in * 997
    numerator = amount_in_with_fee * reserve_out
    denominator = reserve_in * 1000 + amount_in_with_fee
    return numerator // denominator


def triangle_paths(tokens):
    syms = list(tokens)
    for a, b, c in permutations(syms, 3):
        yield (a, b, c, a)


def simulate_triangle(path: Tuple[str, str, str, str], amount_in: int) -> int:
    amount = amount_in
    for i in range(3):
        sym_in = path[i]
        sym_out = path[i+1]
        addr_in = TOKENS[sym_in]["addr"]
        addr_out = TOKENS[sym_out]["addr"]
        pair = pair_address(addr_in, addr_out)
        reserves = get_reserves(pair)
        if reserves is None:
            return 0
        reserve0, reserve1 = reserves
        if addr_in.lower() < addr_out.lower():
            amount = amount_out_v2(amount, reserve0, reserve1)
        else:
            amount = amount_out_v2(amount, reserve1, reserve0)
        if amount == 0:
            return 0
    return amount


def build_call_data(path_syms: Tuple[str, str, str, str], amount_in: int):
    # Build three individual swapExactTokensForTokens calls (UniV2 router)
    targets = []
    payloads = []
    approvals = []
    for i in range(3):
        sym_in = path_syms[i]
        sym_out = path_syms[i+1]
        path = [TOKENS[sym_in]["addr"], TOKENS[sym_out]["addr"]]
        calldata = router_contract.encodeABI(fn_name="swapExactTokensForTokens", args=[amount_in if i == 0 else 0, 0, path, ARBITRAGE_ADDR, int(time.time()) + 600])
        targets.append(ROUTER_V2)
        payloads.append(calldata)
        approvals.append(TOKENS[sym_in]["addr"])
    return targets, payloads, approvals


def main():
    base = "USDC"
    decimals = TOKENS[base]["decimals"]
    init_amount = 1_000_000 * 10 ** decimals  # 1M USDC
    while True:
        best = None
        for p in triangle_paths(TOKENS.keys()):
            if p[0] != base:
                continue
            out_amt = simulate_triangle(p, init_amount)
            if out_amt > init_amount:
                net = out_amt - init_amount
                roi = net / init_amount
                if roi > 0.002:
                    if not best or net > best["net"]:
                        best = {"path": p, "out": out_amt, "net": net, "roi": roi}
        if best:
            print("Best path", best)
            targets, payloads, approvals = build_call_data(best["path"], init_amount)
            min_profit = int(best["net"] * 0.5)  # ask for half expected as safety margin

            calldata = w3.encodeABI(
                fn_name="executeArb",
                args=[
                    TOKENS[base]["addr"],
                    init_amount,
                    targets,
                    payloads,
                    approvals,
                    min_profit,
                ],
                abi=[  # minimal ABI fragment
                    {
                        "name": "executeArb",
                        "type": "function",
                        "inputs": [
                            {"type": "address"},
                            {"type": "uint256"},
                            {"type": "address[]"},
                            {"type": "bytes[]"},
                            {"type": "address[]"},
                            {"type": "uint256"},
                        ],
                        "outputs": [],
                        "stateMutability": "nonpayable",
                    }
                ],
            )
            tx = {
                "to": ARBITRAGE_ADDR,
                "from": mev_signer.address,
                "data": calldata,
                "gas": 0,
                "maxFeePerGas": w3.toWei("50", "gwei"),
                "maxPriorityFeePerGas": w3.toWei("2", "gwei"),
                "chainId": 1,
            }
            gas_est = w3.eth.estimate_gas(tx)
            tx["gas"] = gas_est
            sim = w3.eth.call(tx)
            print("Simulation success", sim.hex()[:10])

            signed_tx = mev_signer.sign_transaction(tx)
            bundle = [signed_tx.rawTransaction]
            block = w3.eth.block_number + 1
            result = fb_w3.flashbots.send_bundle({
                "txs": bundle,
                "block_number": block,
                "min_timestamp": 0,
                "max_timestamp": 0,
                "reverting_tx_hashes": []
            })
            print("Flashbots result", result)
        time.sleep(3)


if __name__ == "__main__":
    main()