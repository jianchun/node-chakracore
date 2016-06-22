{
  'variables': {
    'target_arch%': 'ia32',
    'library%': 'static_library',     # build chakracore as static library or dll
    'component%': 'static_library',   # link crt statically or dynamically
    'chakra_dir%': 'core',
    'msvs_windows_target_platform_version_prop': '',

    'conditions': [
      ['target_arch=="ia32"', { 'Platform': 'x86' }],
      ['target_arch=="x64"', { 'Platform': 'x64' }],
      ['target_arch=="arm"', {
        'Platform': 'arm',
        'msvs_windows_target_platform_version_prop':
          '/p:WindowsTargetPlatformVersion=$(WindowsTargetPlatformVersion)',
      }],
    ],
  },

  'targets': [
    {
      'target_name': 'chakracore',
      'toolsets': ['host'],
      'type': 'none',

      'variables': {
        'chakracore_header': [
          '<(chakra_dir)/lib/Jsrt/ChakraCore.h',
          '<(chakra_dir)/lib/Jsrt/ChakraCommon.h',
          '<(chakra_dir)/lib/Jsrt/ChakraDebug.h',
        ],
        'chakracore_win_bin_dir':
          '<(chakra_dir)/build/vcbuild/bin/<(Platform)_$(ConfigurationName)',
        'conditions': [
          ['OS=="win"', {
            'chakracore_input': '<(chakra_dir)/build/Chakra.Core.sln',
            'chakracore_binaries': [
              '<(chakracore_win_bin_dir)/chakracore.dll',
              '<(chakracore_win_bin_dir)/chakracore.pdb',
              '<(chakracore_win_bin_dir)/chakracore.lib',
            ],
          }, {
            'chakracore_input': '<(chakra_dir)/build.sh',
            'chakracore_binaries': [
              '<(chakra_dir)/BuildLinux/Release/libChakraCore.so',
            ],
          }],
        ],
      },

      'actions': [
        {
          'action_name': 'build_chakracore',
          'inputs': [
            '<(chakracore_input)',
          ],
          'outputs': [
            '<@(chakracore_binaries)',
          ],
          'conditions': [
            ['OS=="win"', {
              'action': [
                'msbuild',
                '/p:Platform=<(Platform)',
                '/p:Configuration=$(ConfigurationName)',
                '/p:RuntimeLib=<(component)',
                '<(msvs_windows_target_platform_version_prop)',
                '/m',
                '<@(_inputs)',
              ],
            }, {
              'action': [ 'bash', '<(chakra_dir)/build.sh', '-j' ],
            }],
          ],
        },
      ],

      'copies': [
        {
          'destination': 'include',
          'files': [ '<@(chakracore_header)' ],
        },
        {
          'destination': '<(PRODUCT_DIR)',
          'files': [ '<@(chakracore_binaries)' ],
        },
      ],

      'direct_dependent_settings': {
        'library_dirs': [ '<(PRODUCT_DIR)' ],
      },

    }, # end chakracore
  ],
}
