i386_ss = ss.source_set()
i386_ss.add(files(
  'fw_cfg.c',
  'e820_memory_layout.c',
  'monitor.c',
  'multiboot.c',
  'x86.c',
  'x86-cpu.c',
))

i386_ss.add(when: 'CONFIG_APIC', if_true: files('vapic.c'))
i386_ss.add(when: 'CONFIG_X86_IOMMU', if_true: files('x86-iommu.c'),
                                      if_false: files('x86-iommu-stub.c'))
i386_ss.add(when: 'CONFIG_AMD_IOMMU', if_true: files('amd_iommu.c'),
                                      if_false: files('amd_iommu-stub.c'))
i386_ss.add(when: 'CONFIG_I440FX', if_true: files('pc_piix.c'))
i386_ss.add(when: 'CONFIG_MICROVM', if_true: files('x86-common.c', 'microvm.c', 'acpi-microvm.c', 'microvm-dt.c'))
i386_ss.add(when: 'CONFIG_NITRO_ENCLAVE', if_true: files('nitro_enclave.c'))
i386_ss.add(when: 'CONFIG_Q35', if_true: files('pc_q35.c'))
i386_ss.add(when: 'CONFIG_VIA_PC', if_true: files('pc_via.c'))
i386_ss.add(when: 'CONFIG_VMMOUSE', if_true: files('vmmouse.c'))
i386_ss.add(when: 'CONFIG_VMPORT', if_true: files('vmport.c'))
i386_ss.add(when: 'CONFIG_VTD', if_true: files('intel_iommu.c'))
i386_ss.add(when: 'CONFIG_SGX', if_true: files('sgx-epc.c','sgx.c'),
                                if_false: files('sgx-stub.c'))

i386_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-common.c'))
i386_ss.add(when: 'CONFIG_PC', if_true: files(
  'x86-common.c',
  'pc.c',
  'pc_sysfw.c',
  'acpi-build.c',
  'port92.c'))
i386_ss.add(when: 'CONFIG_X86_FW_OVMF', if_true: files('pc_sysfw_ovmf.c'),
                                        if_false: files('pc_sysfw_ovmf-stubs.c'))
i386_ss.add(when: 'CONFIG_TDX', if_true: files('tdvf.c', 'tdvf-hob.c'))

subdir('kvm')
subdir('xen')

i386_ss.add_all(xenpv_ss)

hw_arch += {'i386': i386_ss}
