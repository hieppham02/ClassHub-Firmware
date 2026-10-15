const fs = require('node:fs')
const { parse, compileScript, compileTemplate } = require('E:/EAUT/Khoi_nghiep_sang_tao/ClassHub/Frontend/node_modules/@vue/compiler-sfc')
const filename = 'E:/EAUT/Khoi_nghiep_sang_tao/ClassHub/Frontend/src/components/shared/QrAccessPanel.vue'
const { descriptor, errors } = parse(fs.readFileSync(filename, 'utf8'), { filename })
if (errors.length) throw errors[0]
compileScript(descriptor, { id: 'camera-check' })
const template = compileTemplate({ source: descriptor.template.content, filename, id: 'camera-check' })
if (template.errors.length) throw template.errors[0]
console.log('Vue script and template compile OK')
