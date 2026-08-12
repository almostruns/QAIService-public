CREATE TABLE IF NOT EXISTS chat_conversations (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NOT NULL,
  conversation_mode ENUM('general', 'private') NOT NULL,
  title VARCHAR(160) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL DEFAULT '新对话',
  created_at_ms BIGINT NOT NULL,
  updated_at_ms BIGINT NOT NULL,
  PRIMARY KEY (id),
  KEY chat_conversations_user_mode_updated (user_id, conversation_mode, updated_at_ms, id),
  CONSTRAINT chat_conversations_user_foreign FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB;

INSERT INTO chat_conversations (user_id, conversation_mode, title, created_at_ms, updated_at_ms)
SELECT messages.user_id, messages.conversation_mode, '历史对话',
       MIN(messages.occurred_at_ms), MAX(messages.occurred_at_ms)
FROM chat_messages AS messages
LEFT JOIN chat_conversations AS conversations
  ON conversations.user_id = messages.user_id
  AND conversations.conversation_mode = messages.conversation_mode
WHERE conversations.id IS NULL
GROUP BY messages.user_id, messages.conversation_mode;

SET @conversation_column_exists = (
  SELECT COUNT(*) FROM information_schema.columns
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND column_name = 'conversation_id'
);
SET @add_conversation_column = IF(
  @conversation_column_exists = 0,
  'ALTER TABLE chat_messages ADD COLUMN conversation_id BIGINT UNSIGNED NULL AFTER conversation_mode',
  'SELECT 1'
);
PREPARE conversation_statement FROM @add_conversation_column;
EXECUTE conversation_statement;
DEALLOCATE PREPARE conversation_statement;

UPDATE chat_messages AS messages
JOIN (
  SELECT user_id, conversation_mode, MIN(id) AS conversation_id
  FROM chat_conversations
  GROUP BY user_id, conversation_mode
) AS legacy
  ON legacy.user_id = messages.user_id
  AND legacy.conversation_mode = messages.conversation_mode
SET messages.conversation_id = legacy.conversation_id
WHERE messages.conversation_id IS NULL;

SET @conversation_column_nullable = (
  SELECT COUNT(*) FROM information_schema.columns
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND column_name = 'conversation_id'
    AND is_nullable = 'YES'
);
SET @make_conversation_required = IF(
  @conversation_column_nullable > 0,
  'ALTER TABLE chat_messages MODIFY COLUMN conversation_id BIGINT UNSIGNED NOT NULL',
  'SELECT 1'
);
PREPARE conversation_statement FROM @make_conversation_required;
EXECUTE conversation_statement;
DEALLOCATE PREPARE conversation_statement;

SET @conversation_sequence_index_exists = (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND index_name = 'chat_messages_conversation_sequence_unique'
);
SET @add_conversation_sequence_index = IF(
  @conversation_sequence_index_exists = 0,
  'ALTER TABLE chat_messages ADD UNIQUE KEY chat_messages_conversation_sequence_unique (conversation_id, message_sequence)',
  'SELECT 1'
);
PREPARE conversation_statement FROM @add_conversation_sequence_index;
EXECUTE conversation_statement;
DEALLOCATE PREPARE conversation_statement;

SET @user_index_exists = (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND index_name = 'chat_messages_user_index'
);
SET @add_user_index = IF(
  @user_index_exists = 0,
  'ALTER TABLE chat_messages ADD KEY chat_messages_user_index (user_id)',
  'SELECT 1'
);
PREPARE conversation_statement FROM @add_user_index;
EXECUTE conversation_statement;
DEALLOCATE PREPARE conversation_statement;

SET @user_mode_sequence_index_exists = (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND index_name = 'chat_messages_user_mode_sequence_unique'
);
SET @drop_user_mode_sequence_index = IF(
  @user_mode_sequence_index_exists > 0,
  'ALTER TABLE chat_messages DROP INDEX chat_messages_user_mode_sequence_unique',
  'SELECT 1'
);
PREPARE conversation_statement FROM @drop_user_mode_sequence_index;
EXECUTE conversation_statement;
DEALLOCATE PREPARE conversation_statement;

SET @conversation_foreign_exists = (
  SELECT COUNT(*) FROM information_schema.table_constraints
  WHERE constraint_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND constraint_name = 'chat_messages_conversation_foreign'
    AND constraint_type = 'FOREIGN KEY'
);
SET @add_conversation_foreign = IF(
  @conversation_foreign_exists = 0,
  'ALTER TABLE chat_messages ADD CONSTRAINT chat_messages_conversation_foreign FOREIGN KEY (conversation_id) REFERENCES chat_conversations (id) ON DELETE CASCADE',
  'SELECT 1'
);
PREPARE conversation_statement FROM @add_conversation_foreign;
EXECUTE conversation_statement;
DEALLOCATE PREPARE conversation_statement;
