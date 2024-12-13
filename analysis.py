import re
from collections import defaultdict

def parse_log(file_path):
    latencies = defaultdict(list)
    with open(file_path, 'r') as file:
        for line in file:
            # Match the specific format of the log entries
            match = re.search(r'(WebSocket message processing|Order placement|Order modification|Order cancellation|Open orders retrieval|Position retrieval|Order book retrieval) latency: ([\d.]+) seconds', line)
            if match:
                command = match.group(1)
                latency = float(match.group(2))
                latencies[command].append(latency)
    return {command: min(times) for command, times in latencies.items()}

# Parse log files
original_best = parse_log('latency_data_original.log')
optimized_best = parse_log('latency_data.log')

# Calculate improvements
improvements = {}
for command in original_best:
    if command in optimized_best:
        original_latency = original_best[command]
        optimized_latency = optimized_best[command]
        improvement = ((original_latency - optimized_latency) / original_latency) * 100
        improvements[command] = improvement

# Print results
print("Best Latency Comparison:")
print(f"{'Command':<30} {'Original (s)':<15} {'Optimized (s)':<15} {'Improvement (%)':<15}")
for command in original_best:
    if command in optimized_best:
        print(f"{command:<30} {original_best[command]:<15.6f} {optimized_best[command]:<15.6f} {improvements[command]:<15.2f}")