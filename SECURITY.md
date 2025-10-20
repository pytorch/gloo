# Security Policy

gloo best security practices largely follow the same ones as for PyTorch Distributed. Please see https://github.com/pytorch/pytorch/blob/main/SECURITY.md for the most up to date security practices.

## Reporting Security Issues

Beware that none of the topics under [Using Gloo Securely](#using-gloo-securely) are considered vulnerabilities of Gloo.

However, if you believe you have found a security vulnerability in Gloo, we encourage you to let us know right away. We will investigate all legitimate reports and do our best to quickly fix the problem.

Vulnerabilities that are exploitable via the network such as remote code execution should be reported. Issues that require access to the sending/receiving machine or are caused by incorrectly using the API are not considered vulnerabilities in Gloo.

Please report security issues via https://bugbounty.meta.com or by filing an issue for low risk vulnerabilities.

Please refer to the following page for our responsible disclosure policy, reward guidelines, and those things that should not be reported:

https://bugbounty.meta.com
 
## Using Gloo Securely

The only way to guarantee safety with Gloo is to run it in a trusted environment with trusted inputs. Gloo has not been security hardened and bugs as well as misusages of Gloo may result in remote code execution and data leakage.

For performance reasons, most users of Gloo (such as PyTorch) do not use any authorization protocol and will send messages unencrypted. They accept connections from anywhere, and execute the workload sent without performing any checks. Therefore, if you run a Gloo based program on your network, anybody with access to the network may be able to execute arbitrary code and access your data.

If you have a usecase where you do need higher levels of security, Gloo does support TLS but provides no guarantees of security or validity of authorization.

When calling Gloo APIs it is up to the user to validate that the inputs are safe and correct. Invalid inputs to Gloo may result in buffer overflows or other security related issues.
