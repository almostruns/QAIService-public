SET @mode_column_exists = (
  SELECT COUNT(*) FROM information_schema.columns
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND column_name = 'conversation_mode'
);
SET @add_mode_column = IF(
  @mode_column_exists = 0,
  'ALTER TABLE chat_messages ADD COLUMN conversation_mode ENUM(''general'', ''private'') NOT NULL DEFAULT ''general'' AFTER user_id',
  'SELECT 1'
);
PREPARE chat_mode_statement FROM @add_mode_column;
EXECUTE chat_mode_statement;
DEALLOCATE PREPARE chat_mode_statement;

SET @mode_index_exists = (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND index_name = 'chat_messages_user_mode_sequence_unique'
);
-- 006 会把该键替换为 (conversation_id, message_sequence)；
-- 若 006 已执行（会话唯一键已存在），005 重跑时必须跳过，否则同一 (user_id, mode, sequence) 会因多会话重复而 1062。
SET @conversation_index_exists = (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND index_name = 'chat_messages_conversation_sequence_unique'
);
SET @add_mode_index = IF(
  @mode_index_exists = 0 AND @conversation_index_exists = 0,
  'ALTER TABLE chat_messages ADD UNIQUE KEY chat_messages_user_mode_sequence_unique (user_id, conversation_mode, message_sequence)',
  'SELECT 1'
);
PREPARE chat_mode_statement FROM @add_mode_index;
EXECUTE chat_mode_statement;
DEALLOCATE PREPARE chat_mode_statement;

SET @old_index_exists = (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE()
    AND table_name = 'chat_messages'
    AND index_name = 'chat_messages_user_sequence_unique'
);
SET @drop_old_index = IF(
  @old_index_exists > 0,
  'ALTER TABLE chat_messages DROP INDEX chat_messages_user_sequence_unique',
  'SELECT 1'
);
PREPARE chat_mode_statement FROM @drop_old_index;
EXECUTE chat_mode_statement;
DEALLOCATE PREPARE chat_mode_statement;
