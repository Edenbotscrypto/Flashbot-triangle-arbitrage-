import os, json, math, time, asyncio
from itertools import permutations
from web3 import Web3
from eth_abi import encode_abi
from eth_account.signers.local import LocalAccount
from flashbots import flashbot

# --- ENV ---
RPC      = os.environ.get("RPC_URL")
PRIVATE  = os.environ.get("PRIVATE_KEY")
FLASHBOTS_RELAY = os.environ.get("FLASHBOTS_RELAY", "https://relay.flashbots.net")

AAVE_PROVIDER = Web3.toChecksumAddress(os.environ["AAVE_PROVIDER"])
ARBITRAGE_ADDR = Web3.toChecksumAddress(os.environ["ARBITRAGE_ADDR"])  # deploy address

w3 = Web3(Web3.HTTPProvider(RPC))
account: LocalAccount = w3.eth.account.from_key(PRIVATE)
flashbot(w3, account, FLASHBOTS_RELAY)

# --- Token list ---
TOKENS = {
    'USDC':  { 'addr': Web3.toChecksumAddress('0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48'), 'decimals': 6 },
    'DAI':   { 'addr': Web3.toChecksumAddress('0x6B175474E89094C44Da98b954EedeAC495271d0F'), 'decimals': 18},
    'WETH':  { 'addr': Web3.toChecksumAddress('0xC02aaA39b223FE8D0a0e5C4F27eAD9083C756Cc2'), 'decimals': 18},
    'USDT':  { 'addr': Web3.toChecksumAddress('0xdAC17F958D2ee523a2206206994597C13D831ec7'), 'decimals': 6 },
}

# Routers
UNISWAP_V2_ROUTER = Web3.toChecksumAddress(os.environ.get('UNISWAP_V2_ROUTER', '0x7a250d5630B4cF539739dF2C5dAcb4c659F2488D'))
SUSHI_ROUTER     = Web3.toChecksumAddress(os.environ.get('SUSHISWAP_ROUTER', '0xd9e1cE17f2641f24aE83637ab66a2cca9C378B9F'))
CURVE_POOL       = Web3.toChecksumAddress(os.environ.get('CURVE_POOL', '0xDC24316b9AE028F1497c275EB9192a3Ea0f67022'))  # stETH/ETH example

V2PAIR_ABI = json.loads('[{"constant":true,"inputs":[],"name":"getReserves","outputs":[{"internalType":"uint112","name":"reserve0","type":"uint112"},{"internalType":"uint112","name":"reserve1","type":"uint112"},{"internalType":"uint32","name":"blockTimestampLast","type":"uint32"}],"payable":false,"stateMutability":"view","type":"function"}]')

router_abi = json.loads('[{"name":"swapExactTokensForTokens","type":"function","inputs":[{"name":"amountIn","type":"uint256"},{"name":"amountOutMin","type":"uint256"},{"name":"path","type":"address[]"},{"name":"to","type":"address"},{"name":"deadline","type":"uint256"}],"outputs":[{"type":"uint256[]","name":"amounts"}],"stateMutability":"nonpayable"}]')

pair_cache = {}

FEE_V2 = 0.003

def get_pair(tokenA, tokenB):
    key = tuple(sorted([tokenA, tokenB]))
    if key in pair_cache: return pair_cache[key]
    factory = Web3.toChecksumAddress('0x5C69bEe701ef814a2B6a3EDD4B1652CB9cc5aA6f')  # UniswapV2
    salt = Web3.solidityKeccak(['address', 'address'], key)
    init_code = '0x96e8ac4277198ff8b6f785478aa9a39f403cb768dd43259a59e86'  # skipped for brevity
    # placeholder pair address (would compute CREATE2); for demo call external REST maybe
    pair_addr = Web3.toChecksumAddress('0x0000000000000000000000000000000000000000')
    pair_cache[key] = pair_addr
    return pair_addr

def reserve_v2(tokenA, tokenB):
    pair = get_pair(tokenA, tokenB)
    if pair == '0x0000000000000000000000000000000000000000':
        return None
    pair_c = w3.eth.contract(address=pair, abi=V2PAIR_ABI)
    r = pair_c.functions.getReserves().call()
    return r

def amount_out_v2(amount_in, reserve_in, reserve_out):
    amount_in_w_fee = amount_in * 997
    return amount_in_w_fee * reserve_out // (reserve_in * 1000 + amount_in_w_fee)

def triangle_paths(tokens):
    for a,b,c in permutations(tokens, 3):
        yield (a,b,c,a)

def simulate_path(path, amount_in):
    amount = amount_in
    for i in range(3):
        t_in = TOKENS[path[i]]['addr']
        t_out = TOKENS[path[i+1]]['addr']
        reserves = reserve_v2(t_in, t_out)
        if not reserves: return 0
        reserve0, reserve1, _ = reserves
        if t_in.lower() < t_out.lower():
            amount = amount_out_v2(amount, reserve0, reserve1)
        else:
            amount = amount_out_v2(amount, reserve1, reserve0)
    return amount

def gas_price():
    return w3.eth.gas_price

def main():
    base = 'USDC'
    init_amt = 1_000_000 * 10**TOKENS[base]['decimals']
    while True:
        best = None
        for path in triangle_paths(TOKENS.keys()):
            if path[0] != base: continue
            out = simulate_path(path, init_amt)
            if out > init_amt:
                profit = out - init_amt
                if not best or profit > best[2]:
                    best = (path, out, profit)
        if best:
            path, out, profit = best
            print('Arb', path, 'profit', profit)
            # TODO: estimate gas, build calldata, flashbots bundle
        time.sleep(2)

if __name__ == '__main__':
    main()