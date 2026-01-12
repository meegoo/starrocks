file_path = '/workspace/fe/fe-core/src/main/java/com/starrocks/service/FrontendServiceImpl.java'
with open(file_path, 'r') as f:
    content = f.read()

old_field = """    private static final Logger LOG = LogManager.getLogger(FrontendServiceImpl.class);
    private final LeaderImpl leaderImpl;"""

new_field = """    private static final Logger LOG = LogManager.getLogger(FrontendServiceImpl.class);

    // Pending partition creation requests: Key = "tableId_sortedPartitionNames" -> CompletableFuture
    // Used to deduplicate concurrent identical partition creation requests
    private static final ConcurrentHashMap<String, CompletableFuture<TCreatePartitionResult>>
            PENDING_PARTITION_REQUESTS = new ConcurrentHashMap<>();

    private final LeaderImpl leaderImpl;"""

content = content.replace(old_field, new_field)
with open(file_path, 'w') as f:
    f.write(content)

print("Static field added successfully")
