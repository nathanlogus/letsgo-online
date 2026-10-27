from datetime import datetime
from typing import Annotated, Literal
from uuid import UUID
from pydantic import BaseModel, ConfigDict, Field, StringConstraints, model_validator

Label = Annotated[str, StringConstraints(strip_whitespace=True, min_length=1, max_length=64, pattern=r"^[^\x00-\x1f\x7f]+$")]
Adapter = Annotated[str, StringConstraints(pattern=r"^[a-zA-Z0-9_.-]{1,64}$")]


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class TokenInput(Input):
    token: str = Field(min_length=1, max_length=16384)


class RefreshInput(Input):
    refresh_token: str = Field(min_length=1, max_length=512)


class NativeCompleteInput(Input):
    code: str = Field(min_length=1, max_length=4096)
    code_verifier: str = Field(pattern=r"^[A-Za-z0-9._~-]{43,128}$")
    nonce: str = Field(pattern=r"^[A-Za-z0-9_-]{32,128}$")


class ProfileInput(Input):
    id: UUID
    edition: Literal["pikachu", "eevee"]
    label: Label


# Items a gift may give (207): PKHeX's legal Let's Go items without TMs and key items; the same list as the game side's
# (runtime/hle/gift.c giftable[]). The game itself takes any id, so this is the check.
GIFTABLE = frozenset({
    1, 2, 3, 4, 12, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 38, 39, 40, 41, 50, 51,
    53, 55, 56, 57, 58, 59, 60, 61, 62, 76, 77, 78, 79, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93,
    101, 102, 103, 121, 122, 123, 124, 125, 126, 127, 128, 164, 166, 168, 571, 656, 659, 660, 661, 662, 663,
    671, 672, 675, 676, 678, 679, 709, 760, 762, 770, 795, 796, 849, 861, 862, 863, 864, 865, 866, 885, 886,
    887, 888, 889, 890, 891, 892, 893, 894, 895, 896, 900, 901, 902, 903, 960, 961, 962, 963, 964, 965, 966,
    967, 968, 969, 970, 971, 972, 973, 974, 975, 976, 977, 978, 979, 980, 981, 982, 983, 984, 985, 986, 987,
    988, 989, 990, 991, 992, 993, 994, 995, 996, 997, 998, 999, 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007,
    1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025,
    1026, 1027, 1028, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043,
    1044, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057,
})


class Item(Input):
    item_id: int = Field(strict=True, ge=1, le=65535)
    quantity: int = Field(strict=True, ge=1, le=999)

    @model_validator(mode="after")
    def giftable(self):
        if self.item_id not in GIFTABLE:
            raise ValueError("Not a giftable Let's Go item")
        return self


class Reward(Input):
    schema_version: Literal[1] = 1
    kind: Literal["item_bundle"] = "item_bundle"
    items: list[Item] = Field(min_length=1, max_length=32)

    @model_validator(mode="after")
    def unique_items(self):
        if len({i.item_id for i in self.items}) != len(self.items):
            raise ValueError("Duplicate item IDs")
        return self


ALOLAN = {19, 20, 26, 27, 28, 37, 38, 50, 51, 52, 53, 74, 75, 76, 88, 89, 103, 105}  # (form 1 in Let's Go)
Stat = Annotated[int, Field(strict=True, ge=-1, le=31)]  # -1: random


class Pokemon(Input):
    """One Pokemon as a Let's Go card (WB7) describes it; the game creates it on receipt (trainer = the player)."""
    species: int = Field(strict=True, ge=1, le=809)
    form: int = Field(default=0, strict=True, ge=0, le=1)
    level: int = Field(strict=True, ge=1, le=100)
    shiny: Literal["never", "random", "always"] = "never"
    nature: int = Field(default=-1, strict=True, ge=-1, le=24)  # -1: random
    gender: Literal["random", "male", "female"] = "random"
    ivs: list[Stat] = Field(default_factory=lambda: [-1] * 6, min_length=6, max_length=6)  # HP Atk Def Spe SpA SpD
    moves: list[Annotated[int, Field(strict=True, ge=1, le=742)]] = Field(min_length=1, max_length=4)  # (a card's moves are the moves)
    ball: int = Field(default=4, strict=True, ge=1, le=26)  # 4: Poke Ball

    @model_validator(mode="after")
    def let_s_go(self):
        if not (self.species <= 151 or self.species in (808, 809)):
            raise ValueError("Species not in Let's Go")
        if self.form and self.species not in ALOLAN:
            raise ValueError("Form not in Let's Go")
        if len(set(self.moves)) != len(self.moves):
            raise ValueError("Duplicate moves")
        return self


class PokemonReward(Input):
    schema_version: Literal[1] = 1
    kind: Literal["pokemon"]
    pokemon: Pokemon


NATIVE_ADAPTER = "lgpe-wb7-v1"  # served as a native Mystery Gift card (claims get a card_id)


class CampaignInput(Input):
    title: str = Field(min_length=1, max_length=120, pattern=r"^[^\x00-\x1f\x7f]+$")
    starts_at: datetime
    ends_at: datetime
    requires_code: bool = False
    editions: list[Literal["pikachu", "eevee"]] = Field(min_length=1, max_length=2)
    adapters: list[Adapter] = Field(min_length=1, max_length=16)
    reward: Annotated[Reward | PokemonReward, Field(discriminator="kind")]
    stock: int | None = Field(default=None, strict=True, ge=1, le=2147483647)

    @model_validator(mode="after")
    def dates(self):
        if self.starts_at.tzinfo is None or self.ends_at.tzinfo is None or self.ends_at <= self.starts_at:
            raise ValueError("An increasing timezone-aware UTC-compatible window is required")
        if len(set(self.editions)) != len(self.editions) or len(set(self.adapters)) != len(self.adapters):
            raise ValueError("Duplicate editions/adapters")
        if self.reward.kind == "pokemon" and self.adapters != [NATIVE_ADAPTER]:
            raise ValueError("A Pokemon is delivered only as a native card (adapter lgpe-wb7-v1)")
        if NATIVE_ADAPTER in self.adapters and self.reward.kind == "item_bundle" and len(self.reward.items) > 6:
            raise ValueError("A native card holds at most 6 items")
        return self


class CodeInput(Input):
    code: str | None = Field(default=None, min_length=6, max_length=80)
    max_uses: int = Field(default=1, strict=True, ge=1, le=2147483647)


class CampaignAction(Input):
    action: Literal["publish", "pause"]


class ClaimInput(Input):
    campaign_id: UUID | None = None
    code: str | None = Field(default=None, min_length=6, max_length=80)
    profile_id: UUID
    adapter: Adapter

    @model_validator(mode="after")
    def selector(self):
        if (self.campaign_id is None) == (self.code is None):
            raise ValueError("Supply exactly one campaign_id or code")
        return self


class AckInput(Input):
    profile_id: UUID
    reward_digest: str = Field(pattern=r"^[a-f0-9]{64}$")
    receipt_digest: str = Field(pattern=r"^[a-f0-9]{64}$")


class CardInput(Input):
    profile_id: UUID
    card_id: int = Field(strict=True, ge=1024, le=1999)


class AccountAction(Input):
    status: Literal["active", "disabled"]
