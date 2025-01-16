# XDP for Windows experimentation

This example is tested with XDP Version 1.0.2.

## Notes
Access to XDP is restricted to SYSTEM and the built-in administrators group by default.
You either have to run your application as Administrator or add your user account to XDP allowed users. See [here](#optional-grant-user-access) how to do it,

## Installation
Grab the XDP for windows installer `xdp-for-windows.1.0.2.msi` from [XDP for Windows 1.0.2](https://github.com/microsoft/xdp-for-windows/releases/tag/v1.0.2) and install it.

### Optional: Grant user access
Access to XDP is restricted to SYSTEM and the built-in administrators group by default.
The xdpcfg.exe tool can be used to add or remove privileges.

For example, to grant access to SYSTEM, built-in administrators, and the current user do:
- Get your SID  
    1. Open a command shell (Not administrative) to get your users SID:
        ```
        C:\Users\phili>whoami /user

        USER INFORMATION
        ----------------

        User Name             SID
        ===================== ==============================================
        machine-xyz\user      S-1-5-XX-XXXXXXXXXX-XXXXXXXXXX-XXXXXXXXXX-XXXX
        ```

    2. Copy the entire SID string (The string in the form of **S-1-5-XX-XXXXXXXXXX-XXXXXXXXXX-XXXXXXXXXX-XXXX**) from the output.

- Apply new permission  
    1. Open an administrative command prompt to apply new persissions  
    2. Prepare the command line
        ```
        xdpcfg.exe SetDeviceSddl "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;S-1-5-XX-XXXXXXXXXX-XXXXXXXXXX-XXXXXXXXXX-XXXX)"
        ```
    2. Replace **S-1-5-XX-XXXXXXXXXX-XXXXXXXXXX-XXXXXXXXXX-XXXX** with the SID from your account
    3, Execute the commnand

    - Success:
        ```
            C:\Windows\System32> xdpcfg.exe SetDeviceSddl "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;S-1-5-XX-XXXXXXXXXX-XXXXXXXXXX-XXXXXXXXXX-XXXX)"
            C:\Windows\System32>
        ```

    - Failure:
        ```
            C:\Windows\System32> xdpcfg.exe SetDeviceSddl "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;S-1-5-XX-XXXXXXXXXX-XXXXXXXXXX-XXXXXXXXXX-XXXX)"
            SetupDiSetClassRegistryPropertyW failed: 0xd
            C:\Windows\System32>
        ```
        Check that the SID is valid.


- The XDP driver must be restarted for these changes to take effect; the configuration is persistent across driver and machine restarts.  
  e.g. Restart the PC

