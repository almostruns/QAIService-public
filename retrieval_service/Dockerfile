FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1
ENV HF_HOME=/models

WORKDIR /opt/qaiservice

COPY retrieval_service/requirements.txt /opt/qaiservice/retrieval_service/requirements.txt
RUN pip install --no-cache-dir --requirement /opt/qaiservice/retrieval_service/requirements.txt

COPY retrieval_service /opt/qaiservice/retrieval_service

RUN useradd --system --create-home retrieval \
    && mkdir -p /models \
    && chown -R retrieval:retrieval /models /opt/qaiservice

USER retrieval

EXPOSE 8090

HEALTHCHECK --interval=15s --timeout=5s --start-period=180s --retries=4 \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8090/health', timeout=4)"

CMD ["uvicorn", "retrieval_service.app:application", "--host", "0.0.0.0", "--port", "8090", "--workers", "1"]
