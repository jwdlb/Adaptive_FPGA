#!/usr/bin/env python3
"""Generate deterministic, valid CSV market-event streams for reference replay."""

import argparse
import random
from pathlib import Path


HEADER = "timestamp_ns,event_type,side,price_ticks,quantity\n"


# Levels is teh quanity at ecah price, stored in a dict with the price as the key and the quantity as the value.
def choose_existing(levels: dict[int, int], generator: random.Random) -> int:
    """Choose an existing price; callers use this only for non-empty sides."""
    # Sorted returns a sorted list of teh keys in levels, then we choose on one of those keys randomly using the generator.
    return generator.choice(sorted(levels))


def generate(seed: int, count: int) -> list[tuple[int, str, str, int, int]]:
    """Create a valid, deterministic stream while maintaining local book state.

    Bids and asks use non-overlapping price ranges, so generated Add operations
    cannot cross the book. Update, Cancel, and Trade only select live levels.
    """
    generator = random.Random(seed)
    books = {"Bid": {}, "Ask": {}}
    result = []
    for index in range(count):
        side = generator.choice(("Bid", "Ask"))
        levels = books[side]
        event_type = "Add" if not levels else generator.choices(    # If it's the first choise then has to be an add event
            ("Add", "Update", "Cancel", "Trade"), weights=(45, 15, 20, 20)   # Arbitatry modelling choice
        )[0]   # To get teh actual event not list of 1 element with the event in.
        if event_type == "Add":
            # Ten price slots per side mirror the bounded C++ reference book.
            # A bid si buy order and ask is a sell order, so highest bid must be lower than lowest ask.
            price = generator.randint(9991, 10000) if side == "Bid" else generator.randint(10002, 10011)
            quantity = generator.randint(1, 1000)

            # Adds quanity to the existing quantity at that price stored in level.
            # Capped at 4,294,967,295 (0xFFFFFFFF) to match the bounded C++ reference book.
            # If the price is not in the levels dict, it will default to 0 and add the quantity to it.
            levels[price] = min(0xFFFFFFFF, levels.get(price, 0) + quantity)

        else:
            price = choose_existing(levels, generator)
            if event_type == "Update":
                quantity = generator.randint(0, 1000)
                if quantity == 0:
                    # Don't need to store a level with 0 quantity, so remove it from the levels dict.
                    del levels[price]
                else:
                    # Straight update to this quantity at this price, so overwrite the existing quantity in the levels dict.
                    levels[price] = quantity
            else:
                quantity = generator.randint(1, levels[price] + 500)
                remaining = levels[price] - min(levels[price], quantity)
                if remaining == 0:
                    del levels[price]
                else:
                    levels[price] = remaining
        result.append((1_000 + index * 10, event_type, side, price, quantity))
    return result


def main() -> None:
    # Dealing with argumnets for the market data csv generator.
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="CSV file to create")
    parser.add_argument("--seed", type=int, default=42, help="deterministic random seed")
    parser.add_argument("--events", type=int, default=1_000_000, help="number of events to generate")
    arguments = parser.parse_args()
    if arguments.events <= 0:
        parser.error("--events must be positive")

    # Creating the events.
    events = generate(arguments.seed, arguments.events)

    # Creating the CSV and writing the events to the file.
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", encoding="utf-8", newline="") as output:
        output.write(HEADER)
        for event in events:
            output.write(",".join(map(str, event)) + "\n")
    print(f"generated v1 CSV: seed={arguments.seed} events={arguments.events} output={arguments.output}")

# Only run main() if this script is executed directly.
if __name__ == "__main__":
    main()
