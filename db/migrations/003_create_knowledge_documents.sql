CREATE TABLE IF NOT EXISTS knowledge_folders (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NOT NULL,
  parent_id BIGINT UNSIGNED NULL,
  name VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (id),
  UNIQUE KEY knowledge_folders_user_parent_name_unique (user_id, parent_id, name),
  CONSTRAINT knowledge_folders_user_foreign FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT knowledge_folders_parent_foreign FOREIGN KEY (parent_id) REFERENCES knowledge_folders (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS knowledge_documents (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NOT NULL,
  folder_id BIGINT UNSIGNED NULL,
  original_name VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  media_type VARCHAR(100) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  storage_key CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  sha256_hex CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  size_bytes BIGINT UNSIGNED NOT NULL,
  status ENUM('queued', 'processing', 'ready', 'failed') NOT NULL DEFAULT 'queued',
  learning_status ENUM('unreviewed', 'learning', 'mastered', 'needs_review') NOT NULL DEFAULT 'unreviewed',
  error_code VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT '',
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (id),
  UNIQUE KEY knowledge_documents_user_sha_unique (user_id, sha256_hex),
  UNIQUE KEY knowledge_documents_storage_key_unique (storage_key),
  CONSTRAINT knowledge_documents_user_foreign FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT knowledge_documents_folder_foreign FOREIGN KEY (folder_id) REFERENCES knowledge_folders (id) ON DELETE SET NULL
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS knowledge_chunks (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  document_id BIGINT UNSIGNED NOT NULL,
  chunk_index INT UNSIGNED NOT NULL,
  page_number INT UNSIGNED NULL,
  content MEDIUMTEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY knowledge_chunks_document_index_unique (document_id, chunk_index),
  CONSTRAINT knowledge_chunks_document_foreign FOREIGN KEY (document_id) REFERENCES knowledge_documents (id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS knowledge_items (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NOT NULL,
  document_id BIGINT UNSIGNED NOT NULL,
  title VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  learning_status ENUM('unreviewed', 'learning', 'mastered', 'needs_review') NOT NULL DEFAULT 'unreviewed',
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (id),
  KEY knowledge_items_user_status_index (user_id, learning_status),
  CONSTRAINT knowledge_items_user_foreign FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE,
  CONSTRAINT knowledge_items_document_foreign FOREIGN KEY (document_id) REFERENCES knowledge_documents (id) ON DELETE CASCADE
) ENGINE=InnoDB;
