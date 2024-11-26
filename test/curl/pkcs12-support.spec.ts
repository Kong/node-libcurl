import 'should'
import https from 'https'
import fs from 'fs'
import path from 'path'
import { Curl } from '../../lib'
import express from 'express'

const p12Path = path.resolve(__dirname, './example.p12')
const p12Password = 'password123'
const serverPort = 3000
const baseUrl = `https://localhost:${serverPort}/`

// Needs NODE_OPTIONS="--openssl-legacy-provider"

describe('HTTPS Server support with old PKCS12 client certificate', () => {
  let curl: Curl
  let server: https.Server

  before((done) => {
    // Create an Express app and start the HTTPS server
    const app = express()

    app.get('/', (req, res) => {
      res.send('Secure server is running!')
    })

    try {
      const pfx = fs.readFileSync(p12Path)

      server = https.createServer(
        {
          pfx: pfx,
          passphrase: p12Password,
        },
        app,
      )

      server.listen(serverPort, () => {
        console.log(`Test server started on ${baseUrl}`)
        done()
      })
    } catch (error) {
      console.error('error:', error)
      done(error)
    }
  })

  after((done) => {
    if (server) {
      server.close(() => {
        console.log('Test server stopped')
        done()
      })
    }
  })

  beforeEach(() => {
    curl = new Curl()
    curl.setOpt('SSL_VERIFYPEER', false) // Disable SSL peer verification for testing
    curl.setOpt('SSL_VERIFYHOST', 0) // Disable hostname verification
  })

  afterEach(() => {
    curl.close()
  })

  it('should connect using client certificate and receive a valid response', (done) => {
    curl.setOpt('URL', baseUrl)
    curl.setOpt('SSLCERTTYPE', 'P12')
    curl.setOpt('SSLCERT', p12Path) // Set the path to the client certificate
    curl.setOpt('KEYPASSWD', p12Password) // Set the client certificate password

    curl.on('end', (status, data) => {
      if (status !== 200) {
        throw new Error(`Invalid status code: ${status}`)
      }
      console.log('data:', data)
      console.log('status:', status)

      const response = data as string
      response.should.be.equal('Secure server is running!')

      done()
    })

    curl.on('error', (error) => {
      console.error('error-2:', error)
      done(error)
    })
    curl.perform()
  })
})
