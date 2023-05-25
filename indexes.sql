create index block_index on ton_block (seqno, workchain);
create index lt_index on ton_transaction (logical_time desc);