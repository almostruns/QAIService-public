import os
from numbers import Real

from fastapi import FastAPI, HTTPException

from retrieval_service.core import (
    RerankEngine,
    RerankValidationError,
    TokenEngine,
    TokenValidationError,
)


class FlagEmbeddingReranker:
    def __init__(self, model_name: str):
        from FlagEmbedding import FlagReranker

        self.name = model_name
        self._model = FlagReranker(model_name, use_fp16=False)

    def compute_scores(self, query: str, passages: list[str]) -> list[float]:
        pairs = [[query, passage] for passage in passages]
        raw_scores = self._model.compute_score(pairs, normalize=True)
        if isinstance(raw_scores, Real):
            return [float(raw_scores)]
        return [float(score) for score in raw_scores]


class TiktokenEncoding:
    name = "cl100k_base"

    def __init__(self):
        import tiktoken

        self._encoding = tiktoken.get_encoding(self.name)

    def encode(self, text: str) -> list[int]:
        return self._encoding.encode(text, disallowed_special=())

    def decode(self, tokens: list[int]) -> str:
        return self._encoding.decode(tokens)

    def token_bytes(self, token: int) -> bytes:
        return self._encoding.decode_single_token_bytes(token)


def create_app(engine: RerankEngine, token_engine: TokenEngine) -> FastAPI:
    app = FastAPI(title="QAIService Retrieval", docs_url=None, redoc_url=None)

    @app.get("/health")
    def health() -> dict:
        return {"status": "ok", "model": engine.model.name, "encoding": token_engine.encoding.name}

    @app.post("/rerank")
    def rerank(payload: dict) -> dict:
        try:
            return engine.rerank(payload.get("query"), payload.get("candidates"))
        except RerankValidationError as error:
            raise HTTPException(status_code=400, detail=str(error)) from error
        except RuntimeError as error:
            raise HTTPException(status_code=503, detail=str(error)) from error

    @app.post("/chunks")
    def chunks(payload: dict) -> dict:
        try:
            return token_engine.split(
                payload.get("text"),
                payload.get("chunk_tokens", 800),
                payload.get("overlap_tokens", 400),
            )
        except TokenValidationError as error:
            raise HTTPException(status_code=400, detail=str(error)) from error

    @app.post("/fit")
    def fit(payload: dict) -> dict:
        try:
            return token_engine.fit(payload.get("texts"), payload.get("maximum_tokens", 4000))
        except TokenValidationError as error:
            raise HTTPException(status_code=400, detail=str(error)) from error

    return app


MODEL_NAME = os.getenv("QAI_RERANK_MODEL", "BAAI/bge-reranker-v2-m3")
application = create_app(RerankEngine(FlagEmbeddingReranker(MODEL_NAME)), TokenEngine(TiktokenEncoding()))
