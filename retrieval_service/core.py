from bisect import bisect_left, bisect_right
from dataclasses import dataclass
from typing import Protocol


MAXIMUM_CANDIDATES = 64
MAXIMUM_CANDIDATE_BYTES = 2000
MAXIMUM_QUERY_BYTES = 4096


class RerankValidationError(ValueError):
    pass


class TokenValidationError(ValueError):
    pass


class RerankModel(Protocol):
    name: str

    def compute_scores(self, query: str, passages: list[str]) -> list[float]:
        pass


class TextEncoding(Protocol):
    name: str

    def encode(self, text: str) -> list[int]:
        pass

    def decode(self, tokens: list[int]) -> str:
        pass

    def token_bytes(self, token: int) -> bytes:
        pass


@dataclass
class TokenEngine:
    encoding: TextEncoding

    def _safe_token_boundaries(self, text: str, tokens: list[int]) -> list[int]:
        source_bytes = text.encode("utf-8")
        character_boundaries = {0}
        byte_offset = 0
        for character in text:
            byte_offset += len(character.encode("utf-8"))
            character_boundaries.add(byte_offset)

        token_boundaries = [0]
        encoded_bytes = bytearray()
        for token in tokens:
            encoded_bytes.extend(self.encoding.token_bytes(token))
            token_boundaries.append(len(encoded_bytes))
        if bytes(encoded_bytes) != source_bytes:
            raise RuntimeError("encoding_bytes_mismatch")

        return [index for index, offset in enumerate(token_boundaries) if offset in character_boundaries]

    def split(self, text: str, chunk_tokens: int = 800, overlap_tokens: int = 400) -> dict:
        if not isinstance(text, str) or not text:
            raise TokenValidationError("invalid_text")
        if not isinstance(chunk_tokens, int) or chunk_tokens <= 0:
            raise TokenValidationError("invalid_chunk_tokens")
        if not isinstance(overlap_tokens, int) or overlap_tokens < 0 or overlap_tokens >= chunk_tokens:
            raise TokenValidationError("invalid_overlap_tokens")

        tokens = self.encoding.encode(text)
        safe_boundaries = self._safe_token_boundaries(text, tokens)
        step = chunk_tokens - overlap_tokens
        chunks = []
        token_counts = []
        previous_window = None
        for start in range(0, len(tokens), step):
            safe_start_position = bisect_right(safe_boundaries, start) - 1
            safe_start = safe_boundaries[safe_start_position]
            requested_end = min(start + chunk_tokens, len(tokens))
            safe_end_position = bisect_left(safe_boundaries, requested_end)
            safe_end = safe_boundaries[safe_end_position]
            current_window = (safe_start, safe_end)
            if current_window == previous_window:
                continue
            previous_window = current_window
            window = tokens[safe_start:safe_end]
            if not window:
                break
            chunks.append(self.encoding.decode(window))
            token_counts.append(len(window))
            if safe_end >= len(tokens):
                break
        return {"encoding": self.encoding.name, "chunks": chunks, "token_counts": token_counts}

    def fit(self, texts: list[str] | None, maximum_tokens: int = 4000) -> dict:
        if not isinstance(texts, list) or not texts or not all(isinstance(text, str) and text for text in texts):
            raise TokenValidationError("invalid_texts")
        if not isinstance(maximum_tokens, int) or maximum_tokens <= 0:
            raise TokenValidationError("invalid_maximum_tokens")

        fitted = []
        used_tokens = 0
        for text in texts:
            tokens = self.encoding.encode(text)
            remaining_tokens = maximum_tokens - used_tokens
            if remaining_tokens <= 0:
                break
            if len(tokens) <= remaining_tokens:
                fitted.append(text)
                used_tokens += len(tokens)
                continue
            safe_boundaries = self._safe_token_boundaries(text, tokens)
            safe_end_position = bisect_right(safe_boundaries, remaining_tokens) - 1
            safe_end = safe_boundaries[safe_end_position]
            if safe_end == 0:
                break
            fitted.append(self.encoding.decode(tokens[:safe_end]))
            used_tokens += safe_end
            break
        return {"encoding": self.encoding.name, "texts": fitted, "token_count": used_tokens}


@dataclass
class RerankEngine:
    model: RerankModel

    def rerank(self, query: str, candidates: list[dict] | None) -> dict:
        self._validate(query, candidates)
        passages = [candidate["text"] for candidate in candidates]
        scores = self.model.compute_scores(query, passages)
        if len(scores) != len(candidates):
            raise RuntimeError("invalid_model_score_count")
        ranked_scores = []
        for candidate, score in zip(candidates, scores, strict=True):
            ranked_scores.append({"id": candidate["id"], "score": float(score)})
        return {"model": self.model.name, "scores": ranked_scores}

    @staticmethod
    def _validate(query: str, candidates: list[dict] | None) -> None:
        if not isinstance(query, str) or not query.strip():
            raise RerankValidationError("invalid_query")
        if len(query.encode("utf-8")) > MAXIMUM_QUERY_BYTES:
            raise RerankValidationError("query_too_large")
        if not isinstance(candidates, list) or not candidates:
            raise RerankValidationError("invalid_candidates")
        if len(candidates) > MAXIMUM_CANDIDATES:
            raise RerankValidationError("too_many_candidates")
        candidate_ids = set()
        for candidate in candidates:
            if not isinstance(candidate, dict):
                raise RerankValidationError("invalid_candidate")
            candidate_id = candidate.get("id")
            text = candidate.get("text")
            if not isinstance(candidate_id, str) or not candidate_id or not isinstance(text, str) or not text:
                raise RerankValidationError("invalid_candidate")
            if candidate_id in candidate_ids:
                raise RerankValidationError("duplicate_candidate_id")
            if len(text.encode("utf-8")) > MAXIMUM_CANDIDATE_BYTES:
                raise RerankValidationError("candidate_too_large")
            candidate_ids.add(candidate_id)
