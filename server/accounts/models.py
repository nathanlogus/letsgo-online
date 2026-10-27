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


class Item(Input):
    item_id: int = Field(strict=True, ge=1, le=65535)
    quantity: int = Field(strict=True, ge=1, le=999)


class Reward(Input):
    schema_version: Literal[1] = 1
    kind: Literal["item_bundle"] = "item_bundle"
    items: list[Item] = Field(min_length=1, max_length=32)

    @model_validator(mode="after")
    def unique_items(self):
        if len({i.item_id for i in self.items}) != len(self.items):
            raise ValueError("Duplicate item IDs")
        return self


class CampaignInput(Input):
    title: str = Field(min_length=1, max_length=120, pattern=r"^[^\x00-\x1f\x7f]+$")
    starts_at: datetime
    ends_at: datetime
    requires_code: bool = False
    editions: list[Literal["pikachu", "eevee"]] = Field(min_length=1, max_length=2)
    adapters: list[Adapter] = Field(min_length=1, max_length=16)
    reward: Reward
    stock: int | None = Field(default=None, strict=True, ge=1, le=2147483647)

    @model_validator(mode="after")
    def dates(self):
        if self.starts_at.tzinfo is None or self.ends_at.tzinfo is None or self.ends_at <= self.starts_at:
            raise ValueError("An increasing timezone-aware UTC-compatible window is required")
        if len(set(self.editions)) != len(self.editions) or len(set(self.adapters)) != len(self.adapters):
            raise ValueError("Duplicate editions/adapters")
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


class AccountAction(Input):
    status: Literal["active", "disabled"]
