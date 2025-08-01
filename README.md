# R-TCP: A Framework to Optimize TCP Performance Over Rate-Limiting Networks

This repository contains the code for the NSDI '26 paper, "R-TCP: A Framework to Optimize TCP Performance Over Rate-Limiting Networks".

## Table of Contents

- [Overview](#overview)
- [Artifact Evaluation](#artifact-evaluation)
- [Requirements](#requirements)
- [Installation](#installation)
- [Testing](#testing)
- [Configuration](#configuration)
- [Kernel Log Output](#kernel-log-output)

## Overview

R-TCP is a lightweight framework designed for TCP to detect and optimize TCP's performance in moble rate-limited networks. The R-TCP detection algorithm can be applied to different congestion control algorithms. This repository contains the implementation of R-TCP integrated with BBRv1 for single transfer scenario.

This implementation has been extensively tested using mobile rate-limited SIM cards in file downloading scenarios. The [Testing section](#testing) provides detailed instructions on how to evaluate R-TCP-BBRv1 in file downloading scenarios. We are actively collaborating with service provider partners to deploy and test R-TCP at scale in production services.

## Artifact Evaluation

The artifact is intended to earn the "Artifacts Available" badge.

## Requirements

*   Linux Kernel 5.4.0

> **Note:** This code uses `printk` to log debugging information, including estimation and detection results, packet loss counts, and more. It is highly recommended to increase the kernel log buffer size (e.g., to 25) to prevent log overflow.

## Installation

To install the R-TCP-BBRv1 congestion control module, run the provided shell script with superuser privileges:

```bash
sudo sh command.sh
```

The name of the installed congestion control module is `rtcp_bbr`.

## Testing

The congestion control module should be installed on the server-side machine running Linux Kernel 5.4.0.

1.  **Set up a file server:** You can install a web server like Apache to serve files.

2.  **Client Request:** Use another machine as a client to request a file from the server. The client machine can be connected to a mobile rate-limited network by plugging in a mobile modem (e.g., USB modem with a rate-limited SIM card). 
    > **Important:** When testing with mobile rate-limited networks, begin with large files if the token bucket size is unknown. Small files may finish transferring before depleting the token bucket, which can obscure rate-limiting behavior and lead to inconclusive testing outcomes.

3.  **Check Logs:** After the file transfer is complete, you can inspect the kernel log on the server for recorded information from the R-TCP module by running:
    ```bash
    sudo dmesg
    ```

## Configuration

You can dynamically configure the parameters of the R-TCP-BBRv1 congestion control algorithm without needing to reinstall the module. Use the following command format:

```bash
sudo echo {value} | sudo tee /sys/module/rtcp_bbr/parameters/{key}
```

Replace `{value}` with the desired value and `{key}` with the parameter you wish to modify.

### Available Parameters

| Parameter | Description | Default Value |
| :--- | :--- | :--- |
| `probe_interval` | Corresponds to **η** in the paper. The cap increases by **γ%** once every **η** rounds. | `20` |
| `probe_per` | Used to calculate **γ** in the paper via the formula `(probe_per * 5) - 100`. | `24` |
| `optimize_flag` | Toggles performance optimization. `1` enables optimization, `0` disables it. | `1` |
| `enable_printk` | Toggles `printk` logging. `1` enables logging, `0` disables it. | `1` |

## Kernel Log Output

When `printk` is enabled, the module records information to the kernel log. You can view this log by running the `dmesg` command.

### Per-ACK Information

For each processed ACK packet, the following information is logged (this can be commented out if needed):

| Field | Description |
| :--- | :--- |
| `c` | The detection result. `1` indicates that rate limiting was successfully detected; `0` means it has not been detected yet. |
| `B` | If rate limiting is detected, this shows the estimated bucket size. |
| `R` | If rate limiting is detected, this shows the estimated rate limit. |
| `u_p` | If rate limiting is detected, this indicates the cap for the pacing rate, in Bytes per second. |
| `r_p` | The actual pacing rate, in Bytes per second. |
| `n` | `1` means increase the cap by γ%; `0` means no increase. |

### Summary Information

In addition to per-ACK logging, summary information is recorded to the kernel log once every MAX_STR_LEN (default 5000) ACK packets. This summary includes:

- Detection results
- Estimation results
- Aggregated statistics over the monitoring period
